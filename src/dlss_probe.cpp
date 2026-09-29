#include "dlss_probe.h"
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

// [DLSSP] Find the pass that READS the engine's motion vectors, by
// resource identity (see dlss_probe.h).  Nothing here writes engine state, creates a
// resource, or touches the render path.
namespace
{
constexpr std::size_t kTracked = 16;        // velocity / depth resources remembered
constexpr std::size_t kReaders = 32;        // shaders that read a tracked velocity
constexpr std::size_t kPassNames = 128;
constexpr std::size_t kPassNameChars = 48;
constexpr unsigned kMaxFullscreenCount = 6; // full-screen draws: 3/4 verts, 4/6 indices
constexpr UINT kSrvSlots = 16;
constexpr UINT kFullSizeMinWidth = 1024;    // backbuffer family vs dilation/tile buffers
constexpr std::uint32_t kFirstReportPresent = 120;
constexpr std::uint32_t kReportEveryPresents = 600;
constexpr std::uint64_t kMaxJitterSamples = 100000;

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
    case DXGI_FORMAT_R11G11B10_FLOAT: return "RG11B10F";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8s";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8T";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
    case DXGI_FORMAT_R16G16_TYPELESS: return "RG16T";
    case DXGI_FORMAT_R16G16_UNORM: return "RG16U";
    case DXGI_FORMAT_R16G16_SNORM: return "RG16S";
    case DXGI_FORMAT_R8G8_UNORM: return "RG8";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R32_TYPELESS: return "R32T";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24T";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32F_X8X24";
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return "X32_G8X24";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8T";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24U_X8";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32S8";
    case DXGI_FORMAT_D32_FLOAT: return "D32F";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case DXGI_FORMAT_R16_FLOAT: return "R16F";
    case DXGI_FORMAT_R16_UNORM: return "R16U";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    case DXGI_FORMAT_UNKNOWN: return "none";
    default: return "other";
    }
}

// Everything that can hold a lit scene image.  Used only to DESCRIBE what a
// velocity reader also samples; identification never depends on it.
bool IsColourFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

bool IsDepthDsvFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_D32_FLOAT ||
        f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D16_UNORM;
}

// A resource the engine writes as velocity or depth, identified when it is
// bound for OUTPUT.  The key is identity only - never dereferenced.
struct Tracked
{
    std::atomic_uintptr_t key{};
    std::atomic_uint32_t ready{};
    UINT width{}, height{};
    DXGI_FORMAT format{};
    std::atomic_uint32_t mrtSlots{};   // velocity: which MRT slots it was bound to
    std::atomic_uint64_t binds{};
};
std::array<Tracked, kTracked> g_velocityRes{};
std::array<Tracked, kTracked> g_depthRes{};
std::atomic_uint32_t g_trackedOverflow{};

struct Slot
{
    int index = -1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0;
};

// A shader seen reading a tracked velocity resource.
struct Reader
{
    std::atomic_uintptr_t key{};       // PS or CS object, identity within a run
    std::atomic_uint32_t ready{};      // 0 empty, 1 being filled, 2 readable
    bool compute{};
    std::atomic_uint64_t hits{};
    std::atomic_uint64_t hitsEye[2]{};
    std::uint64_t reportedEye[2]{};    // present thread only: interval deltas
    std::atomic_uint32_t hitsThisFrame{};
    std::atomic_uint32_t peakPerFrame{};
    std::atomic_uint32_t withDepth{};  // hits that ALSO read a tracked depth buffer
    std::atomic_uint32_t lastPresent{};
    // Snapshot of the first observation; only read once ready == 2.
    int velocitySlot = -1;
    int depthSlot = -1;
    Slot velocity, depth, colour;
    DXGI_FORMAT rtFormat{};
    UINT rtWidth{}, rtHeight{};
    UINT cbBytes{};
    char pass[kPassNameChars]{};
};
std::array<Reader, kReaders> g_reader{};
std::atomic_uint32_t g_readerOverflow{};

struct PassName
{
    std::atomic_uint32_t ready{};
    char name[kPassNameChars]{};
    std::atomic_uint64_t announcements{};
};
std::array<PassName, kPassNames> g_pass{};
std::atomic_uint32_t g_passCount{};
std::atomic_uint32_t g_passOverflow{};
char g_currentPass[kPassNameChars]{};
bool g_passNamesLogged = false;

bool g_enabled = true;
bool g_initDone = false;
std::atomic_uint32_t g_present{};
// Set once a fingerprinted resolve reader exists; Inspect then runs on one
// present in eight (a new resolve shader is still found within 8 frames).
std::atomic_bool g_fingerprintSeen{false};
constexpr std::uint32_t kInspectEvery = 8;
inline bool InspectThisPresent()
{
    return !g_fingerprintSeen.load(std::memory_order_relaxed) ||
        (g_present.load(std::memory_order_relaxed) % kInspectEvery) == 0;
}
std::atomic_uint32_t g_lastReport{};
std::atomic_bool g_announced{};
std::atomic_uint32_t g_fullscreenThisFrame{}, g_fullscreenPerFrame{};
std::atomic_uint32_t g_dispatchThisFrame{}, g_dispatchPerFrame{};
std::atomic_uint64_t g_velocityReadsDraw{}, g_velocityReadsDispatch{};
std::atomic_uint64_t g_depthReadsDraw{}, g_depthReadsDispatch{};
std::atomic_int g_mode{};        // 0 mono, 1 AER, 2 native stereo
std::atomic_int g_renderEye{};

// Jitter, sampled from the first velocity+depth reader's CB0 through the
// Map/Unmap detours.  Eight bytes off a buffer the engine writes anyway.
std::atomic_uintptr_t g_watchCb{};
std::atomic_uintptr_t g_mapped{};
std::atomic_uint64_t g_jitterSamples{};
std::atomic_uint32_t g_jitterLastX{}, g_jitterLastY{};
float g_jitterMinX = 0.0f, g_jitterMaxX = 0.0f;
float g_jitterMinY = 0.0f, g_jitterMaxY = 0.0f;
bool g_jitterSeen = false;

float BitsToFloat(std::uint32_t bits)
{
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::uint32_t FloatToBits(float f)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
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
            *(slash + 1) = L'\0';
            wchar_t ini[MAX_PATH]{};
            std::wcsncpy(ini, path, MAX_PATH - 1);
            std::wcsncat(ini, L"ffxv-vr.ini", MAX_PATH - 1 - std::wcslen(ini));
            g_enabled = GetPrivateProfileIntW(L"Probe", L"Dlss", 1, ini) != 0;
        }
    }
    Logf("[DLSSP] D1 v3 recon %s (ini [Probe] Dlss). Tracks the velocity (RG16F render "
         "target >=%u px) and depth buffers by RESOURCE IDENTITY as the engine writes them, then "
         "reports every full-screen draw and every compute dispatch that READS them. No format "
         "guessing, no slot assumption.",
        g_enabled ? "ON" : "off", kFullSizeMinWidth);
}

// Resource identity + 2D size behind any view.  The AddRef from GetResource
// is dropped at once; the pointer is kept only as an identity key.
std::uintptr_t ViewResource(ID3D11View* view, UINT* width, UINT* height, bool wantSize)
{
    if (width) *width = 0;
    if (height) *height = 0;
    if (!view)
        return 0;
    ID3D11Resource* resource{};
    view->GetResource(&resource);
    if (!resource)
        return 0;
    const auto key = reinterpret_cast<std::uintptr_t>(resource);
    if (wantSize)
    {
        ID3D11Texture2D* texture{};
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&texture))) &&
            texture)
        {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (width) *width = desc.Width;
            if (height) *height = desc.Height;
            texture->Release();
        }
    }
    resource->Release();
    return key;
}

Tracked* Track(std::array<Tracked, kTracked>& set, std::uintptr_t key, UINT width, UINT height,
    DXGI_FORMAT format)
{
    for (auto& t : set)
    {
        const auto existing = t.key.load(std::memory_order_acquire);
        if (existing == key)
            return &t;
        if (existing == 0)
        {
            std::uintptr_t expected = 0;
            if (t.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel))
            {
                t.width = width;
                t.height = height;
                t.format = format;
                t.ready.store(2, std::memory_order_release);
                return &t;
            }
            if (expected == key)
                return &t;
        }
    }
    g_trackedOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

bool IsTracked(const std::array<Tracked, kTracked>& set, std::uintptr_t key)
{
    if (!key)
        return false;
    for (const auto& t : set)
    {
        const auto existing = t.key.load(std::memory_order_relaxed);
        if (existing == key)
            return true;
        if (existing == 0)
            return false;   // filled in order; first empty slot ends the set
    }
    return false;
}

Reader* ReaderFor(std::uintptr_t key)
{
    for (auto& r : g_reader)
    {
        const auto existing = r.key.load(std::memory_order_acquire);
        if (existing == key)
            return &r;
        if (existing == 0)
        {
            std::uintptr_t expected = 0;
            if (r.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel))
                return &r;
            if (expected == key)
                return &r;
        }
    }
    g_readerOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

const char* ModeName()
{
    switch (g_mode.load(std::memory_order_relaxed))
    {
    case 2: return "native-stereo";
    case 1: return "AER";
    default: return "mono";
    }
}

// Shared by draws (PS) and dispatches (CS).
void Inspect(ID3D11DeviceContext* context, bool compute)
{
    ID3D11ShaderResourceView* srv[kSrvSlots]{};
    if (compute)
        context->CSGetShaderResources(0, kSrvSlots, srv);
    else
        context->PSGetShaderResources(0, kSrvSlots, srv);

    int velocitySlot = -1;
    int depthSlot = -1;
    for (UINT i = 0; i < kSrvSlots; ++i)
    {
        if (!srv[i])
            continue;
        const auto key = ViewResource(srv[i], nullptr, nullptr, false);
        if (velocitySlot < 0 && IsTracked(g_velocityRes, key))
            velocitySlot = static_cast<int>(i);
        else if (depthSlot < 0 && IsTracked(g_depthRes, key))
            depthSlot = static_cast<int>(i);
    }

    if (depthSlot >= 0)
        (compute ? g_depthReadsDispatch : g_depthReadsDraw).fetch_add(1, std::memory_order_relaxed);

    if (velocitySlot >= 0)
    {
        (compute ? g_velocityReadsDispatch : g_velocityReadsDraw)
            .fetch_add(1, std::memory_order_relaxed);

        std::uintptr_t shaderKey = 0;
        if (compute)
        {
            ID3D11ComputeShader* shader{};
            UINT classInstances = 0;
            context->CSGetShader(&shader, nullptr, &classInstances);
            shaderKey = reinterpret_cast<std::uintptr_t>(shader);
            if (shader)
                shader->Release();
        }
        else
        {
            ID3D11PixelShader* shader{};
            UINT classInstances = 0;
            context->PSGetShader(&shader, nullptr, &classInstances);
            shaderKey = reinterpret_cast<std::uintptr_t>(shader);
            if (shader)
                shader->Release();
        }

        Reader* reader = shaderKey ? ReaderFor(shaderKey) : nullptr;
        if (reader)
        {
            reader->hits.fetch_add(1, std::memory_order_relaxed);
            reader->hitsThisFrame.fetch_add(1, std::memory_order_relaxed);
            reader->hitsEye[(g_renderEye.load(std::memory_order_relaxed) == 1) ? 1 : 0]
                .fetch_add(1, std::memory_order_relaxed);
            reader->lastPresent.store(g_present.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            if (depthSlot >= 0)
                reader->withDepth.fetch_add(1, std::memory_order_relaxed);

            std::uint32_t expected = 0;
            if (reader->ready.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            {
                reader->compute = compute;
                reader->velocitySlot = velocitySlot;
                reader->depthSlot = depthSlot;
                std::strncpy(reader->pass, g_currentPass, kPassNameChars - 1);
                // Describe what else it samples: the velocity and depth views
                // plus the largest colour-format input (the likely scene image).
                for (UINT i = 0; i < kSrvSlots; ++i)
                {
                    if (!srv[i])
                        continue;
                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                    srv[i]->GetDesc(&desc);
                    UINT width = 0, height = 0;
                    ViewResource(srv[i], &width, &height, true);
                    const Slot slot{static_cast<int>(i), desc.Format, width, height};
                    if (static_cast<int>(i) == velocitySlot)
                        reader->velocity = slot;
                    else if (static_cast<int>(i) == depthSlot)
                        reader->depth = slot;
                    else if (IsColourFormat(desc.Format) &&
                        (reader->colour.index < 0 || width > reader->colour.width))
                        reader->colour = slot;
                }

                ID3D11Buffer* cb{};
                if (compute)
                    context->CSGetConstantBuffers(0, 1, &cb);
                else
                    context->PSGetConstantBuffers(0, 1, &cb);
                if (cb)
                {
                    D3D11_BUFFER_DESC cbDesc{};
                    cb->GetDesc(&cbDesc);
                    reader->cbBytes = cbDesc.ByteWidth;
                    if (depthSlot >= 0)
                    {
                        std::uintptr_t none = 0;
                        g_watchCb.compare_exchange_strong(none,
                            reinterpret_cast<std::uintptr_t>(cb), std::memory_order_acq_rel);
                    }
                    cb->Release();
                }

                if (!compute)
                {
                    ID3D11RenderTargetView* rtv{};
                    context->OMGetRenderTargets(1, &rtv, nullptr);
                    if (rtv)
                    {
                        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
                        rtv->GetDesc(&rtvDesc);
                        reader->rtFormat = rtvDesc.Format;
                        ViewResource(rtv, &reader->rtWidth, &reader->rtHeight, true);
                        rtv->Release();
                    }
                }
                reader->ready.store(2, std::memory_order_release);

                if (depthSlot >= 0 && !g_announced.exchange(true))
                {
                    Logf("[DLSSP] FIRST VELOCITY+DEPTH READER at present %u: %s %p pass=%s "
                         "velocity=slot%d %s %ux%u depth=slot%d %s %ux%u colour=slot%d %s %ux%u "
                         "cb0=%uB rt=%s %ux%u. Reads the engine's own motion vectors AND scene "
                         "depth: the temporal resolve DLSS would take over.",
                        g_present.load(std::memory_order_relaxed), compute ? "CS" : "PS",
                        reinterpret_cast<void*>(shaderKey), reader->pass[0] ? reader->pass : "?",
                        reader->velocity.index, FormatName(reader->velocity.format),
                        reader->velocity.width, reader->velocity.height,
                        reader->depth.index, FormatName(reader->depth.format),
                        reader->depth.width, reader->depth.height,
                        reader->colour.index, FormatName(reader->colour.format),
                        reader->colour.width, reader->colour.height, reader->cbBytes,
                        FormatName(reader->rtFormat), reader->rtWidth, reader->rtHeight);
                }
            }
        }
    }

    for (UINT i = 0; i < kSrvSlots; ++i)
        if (srv[i])
            srv[i]->Release();
}

void ReportPassNames()
{
    const auto count = g_passCount.load(std::memory_order_acquire);
    Logf("[DLSSP] engine-announced passes: %u (overflow=%u). These are scene layers; the "
         "post-effect filters run between ForwardOnPostEffect and AfterPostEffect WITHOUT their "
         "own announcements, so this list cannot prove or disprove a TAA pass by itself.",
        count, g_passOverflow.load(std::memory_order_relaxed));
    char line[1200];
    std::size_t used = 0;
    line[0] = '\0';
    for (std::size_t i = 0; i < kPassNames && i < count; ++i)
    {
        auto& p = g_pass[i];
        if (p.ready.load(std::memory_order_acquire) != 2)
            continue;
        char item[96];
        const int written = _snprintf_s(item, sizeof(item), _TRUNCATE, "%s ", p.name);
        if (written <= 0)
            continue;
        if (used + static_cast<std::size_t>(written) >= sizeof(line) - 1)
        {
            Logf("[DLSSP]  %s", line);
            line[0] = '\0';
            used = 0;
        }
        std::strncat(line, item, sizeof(line) - used - 1);
        used += static_cast<std::size_t>(written);
    }
    if (used)
        Logf("[DLSSP]  %s", line);
}

void Report()
{
    const auto present = g_present.load(std::memory_order_relaxed);

    std::size_t velocityCount = 0, depthCount = 0;
    char velocityList[400]{};
    for (const auto& t : g_velocityRes)
    {
        if (t.ready.load(std::memory_order_acquire) != 2)
            continue;
        ++velocityCount;
        char item[64];
        _snprintf_s(item, sizeof(item), _TRUNCATE, "%ux%u(mrt 0x%02X) ", t.width, t.height,
            t.mrtSlots.load(std::memory_order_relaxed));
        std::strncat(velocityList, item, sizeof(velocityList) - std::strlen(velocityList) - 1);
    }
    for (const auto& t : g_depthRes)
        if (t.ready.load(std::memory_order_acquire) == 2)
            ++depthCount;

    Logf("[DLSSP] present=%u mode=%s fullscreenDraws/frame=%u dispatches/frame=%u | tracked "
         "velocity=%zu [%s] depth=%zu overflow=%u | velocity READS draw=%llu dispatch=%llu | "
         "depth reads draw=%llu dispatch=%llu",
        present, ModeName(), g_fullscreenPerFrame.load(std::memory_order_relaxed),
        g_dispatchPerFrame.load(std::memory_order_relaxed), velocityCount,
        velocityCount ? velocityList : "none yet", depthCount,
        g_trackedOverflow.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_velocityReadsDraw.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_velocityReadsDispatch.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_depthReadsDraw.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_depthReadsDispatch.load(std::memory_order_relaxed)));

    std::size_t readers = 0, withDepth = 0;
    for (auto& r : g_reader)
    {
        if (r.ready.load(std::memory_order_acquire) != 2)
            continue;
        ++readers;
        const auto depthHits = r.withDepth.load(std::memory_order_relaxed);
        if (depthHits)
            ++withDepth;
        const auto eyeA = r.hitsEye[0].load(std::memory_order_relaxed);
        const auto eyeB = r.hitsEye[1].load(std::memory_order_relaxed);
        Logf("[DLSSP]  %s %p pass=%s hits=%llu (+%llu/+%llu eyeA/eyeB this interval) "
             "peak/frame=%u withDepth=%u velocity=slot%d %s %ux%u depth=slot%d %s %ux%u "
             "colour=slot%d %s %ux%u cb0=%uB rt=%s %ux%u lastPresent=%u",
            r.compute ? "CS" : "PS", reinterpret_cast<void*>(r.key.load(std::memory_order_relaxed)),
            r.pass[0] ? r.pass : "?",
            static_cast<unsigned long long>(r.hits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(eyeA - r.reportedEye[0]),
            static_cast<unsigned long long>(eyeB - r.reportedEye[1]),
            r.peakPerFrame.load(std::memory_order_relaxed), depthHits,
            r.velocity.index, FormatName(r.velocity.format), r.velocity.width, r.velocity.height,
            r.depth.index, FormatName(r.depth.format), r.depth.width, r.depth.height,
            r.colour.index, FormatName(r.colour.format), r.colour.width, r.colour.height,
            r.cbBytes, FormatName(r.rtFormat), r.rtWidth, r.rtHeight,
            r.lastPresent.load(std::memory_order_relaxed));
        r.reportedEye[0] = eyeA;
        r.reportedEye[1] = eyeB;
    }

    const auto samples = g_jitterSamples.load(std::memory_order_relaxed);
    if (samples)
    {
        Logf("[DLSSP]  jitter CB0[8],[9] samples=%llu last=(%.6f, %.6f) x[%.6f..%.6f] "
             "y[%.6f..%.6f] (raw; the reference add-on multiplies these by render w/h)",
            static_cast<unsigned long long>(samples),
            BitsToFloat(g_jitterLastX.load(std::memory_order_relaxed)),
            BitsToFloat(g_jitterLastY.load(std::memory_order_relaxed)),
            g_jitterMinX, g_jitterMaxX, g_jitterMinY, g_jitterMaxY);
    }

    if (!g_passNamesLogged && g_passCount.load(std::memory_order_relaxed) > 0 && velocityCount)
    {
        g_passNamesLogged = true;
        ReportPassNames();
    }

    // Standing verdict, restated every report so the latest line is the answer.
    if (velocityCount == 0)
        Logf("[DLSSP] STATUS: no full-size RG16F render target written yet (menus/loading?). "
             "Nothing to judge until gameplay.");
    else if (readers == 0)
        Logf("[DLSSP] STATUS: the engine WRITES velocity (%zu buffer%s) but NO full-screen draw "
             "and NO compute dispatch has READ it. Nothing in this build consumes motion vectors "
             "for a temporal resolve.", velocityCount, velocityCount == 1 ? "" : "s");
    else if (withDepth == 0)
        Logf("[DLSSP] STATUS: %zu shader%s read velocity, none together with scene depth. "
             "Velocity consumers exist but none looks like a depth-aware temporal resolve.",
            readers, readers == 1 ? "" : "s");
    else
        Logf("[DLSSP] STATUS: %zu shader%s read velocity; %zu also read scene depth - the "
             "temporal-resolve candidate%s listed above.", readers, readers == 1 ? "" : "s",
            withDepth, withDepth == 1 ? " is" : "s are");
}

// ---- [GPUT] GPU frame timer ----------------------------------------------
constexpr int kGpuSlots = 8;                   // frames in flight that can be waited on
constexpr std::uint32_t kGpuWindow = 300;      // presents per report
struct GpuSlot
{
    ID3D11Query* disjoint{};
    ID3D11Query* start{};
    ID3D11Query* end{};
    bool open{};
    bool pending{};
    std::atomic_bool startIssued{};
    UINT renderWidth{};
};
GpuSlot g_gpu[kGpuSlots];
std::atomic_int g_gpuCurrent{-1};
std::atomic_uintptr_t g_gpuImmediate{};        // identity; used on the present thread
std::atomic_bool g_gpuStartIssued{true};
bool g_gpuArmed = false;
bool g_gpuFailed = false;
bool g_gpuHaveLastEnd = false;
std::uint64_t g_gpuLastEnd = 0;
std::atomic_uint32_t g_lastVelocityWidth{};
float g_gpuSpan[kGpuWindow]{};
float g_gpuPeriod[kGpuWindow]{};
std::uint32_t g_gpuSpanCount = 0, g_gpuPeriodCount = 0;
std::uint32_t g_gpuDisjoint = 0, g_gpuStale = 0, g_gpuNoStart = 0;
std::uint32_t g_gpuWindowPresents = 0;
UINT g_gpuWindowWidthMin = 0, g_gpuWindowWidthMax = 0;

void GpuStamp(ID3D11DeviceContext* context)
{
    if (reinterpret_cast<std::uintptr_t>(context) != g_gpuImmediate.load(std::memory_order_relaxed))
        return;
    if (g_gpuStartIssued.load(std::memory_order_relaxed) ||
        g_gpuStartIssued.exchange(true, std::memory_order_acq_rel))
        return;
    const int current = g_gpuCurrent.load(std::memory_order_acquire);
    if (current < 0)
        return;
    auto& slot = g_gpu[current];
    if (!slot.open || !slot.start)
        return;
    context->End(slot.start);
    slot.startIssued.store(true, std::memory_order_release);
}

float Percentile(const float* values, std::uint32_t count, float fraction)
{
    if (!count)
        return 0.0f;
    float sorted[kGpuWindow];
    std::memcpy(sorted, values, count * sizeof(float));
    const auto k = static_cast<std::uint32_t>(fraction * static_cast<float>(count - 1) + 0.5f);
    std::nth_element(sorted, sorted + k, sorted + count);
    return sorted[k];
}

void GpuReport()
{
    const float spanP50 = Percentile(g_gpuSpan, g_gpuSpanCount, 0.5f);
    const float spanP90 = Percentile(g_gpuSpan, g_gpuSpanCount, 0.9f);
    const float periodP50 = Percentile(g_gpuPeriod, g_gpuPeriodCount, 0.5f);
    const float periodP90 = Percentile(g_gpuPeriod, g_gpuPeriodCount, 0.9f);
    Logf("[GPUT] present=%u mode=%s renderW=%u%s gpu span p50/p90=%.2f/%.2f ms period "
         "p50/p90=%.2f/%.2f ms busy=%.0f%% samples=%u disjoint=%u stale=%u noStart=%u",
        g_present.load(std::memory_order_relaxed), ModeName(), g_gpuWindowWidthMax,
        g_gpuWindowWidthMin != g_gpuWindowWidthMax ? "(CHANGED in window)" : "",
        spanP50, spanP90, periodP50, periodP90,
        periodP50 > 0.0f ? 100.0f * spanP50 / periodP50 : 0.0f,
        g_gpuSpanCount, g_gpuDisjoint, g_gpuStale, g_gpuNoStart);
    g_gpuSpanCount = g_gpuPeriodCount = 0;
    g_gpuDisjoint = g_gpuStale = g_gpuNoStart = 0;
    g_gpuWindowPresents = 0;
    g_gpuWindowWidthMin = g_gpuWindowWidthMax = 0;
}

bool GpuArm(IDXGISwapChain* swapChain)
{
    ID3D11Device* device{};
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) ||
        !device)
        return false;
    bool ok = true;
    for (auto& slot : g_gpu)
    {
        D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
        ok = ok && SUCCEEDED(device->CreateQuery(&disjoint, &slot.disjoint)) &&
            SUCCEEDED(device->CreateQuery(&stamp, &slot.start)) &&
            SUCCEEDED(device->CreateQuery(&stamp, &slot.end));
    }
    ID3D11DeviceContext* immediate{};
    device->GetImmediateContext(&immediate);
    if (immediate)
    {
        g_gpuImmediate.store(reinterpret_cast<std::uintptr_t>(immediate), std::memory_order_release);
        immediate->Release();   // identity only; lives as long as the game's device
    }
    device->Release();
    if (!ok || !immediate)
    {
        for (auto& slot : g_gpu)
        {
            if (slot.disjoint) slot.disjoint->Release();
            if (slot.start) slot.start->Release();
            if (slot.end) slot.end->Release();
            slot.disjoint = slot.start = slot.end = nullptr;
        }
        g_gpuImmediate.store(0, std::memory_order_release);
        return false;
    }
    Logf("[GPUT] GPU frame timer armed: %d timestamp slots on the immediate context. span = "
         "first engine command after Present -> Present; period = Present -> Present (GPU "
         "clock); busy = span/period; renderW = current velocity-target width (6144 = 100%%, "
         "3072 = 50%%). Report every %u presents.", kGpuSlots, kGpuWindow);
    return true;
}

} // namespace

namespace DlssProbe
{

std::uintptr_t ImmediateContextKey()
{
    return g_gpuImmediate.load(std::memory_order_acquire);
}

// [DLSSPICK] With TAA switched on AFTER launch, other post
// effects (motion blur / DoF: velocity slot 1, RGBA16F colour, 512-byte CB)
// register as depth+velocity readers before the TAA resolve does, and the old
// "first pixel-shader reader with depth" rule latched one of them: DLSS then
// rejected its inputs every frame and never ran.  Pick the reader that matches
// the resolve's fingerprint (docs/FFXV_ENGINE_REFERENCE.md):
// pixel shader, velocity slot 6, depth slot 3, colour slot 0 RG11B10F, 256-byte
// cbTemporalAA, RG11B10F target.  Fall back to the old rule only if no reader
// matches, so a future engine difference still degrades to the old behaviour.
std::uintptr_t TemporalResolveShader()
{
    std::uintptr_t fallback = 0, best = 0;
    std::uint32_t bestPresent = 0;
    for (const auto& r : g_reader)
    {
        if (r.ready.load(std::memory_order_acquire) != 2)
            continue;
        if (r.compute || r.withDepth.load(std::memory_order_relaxed) == 0)
            continue;
        const bool fingerprint = r.velocitySlot == 6 && r.depthSlot == 3 &&
            r.colour.format == DXGI_FORMAT_R11G11B10_FLOAT &&
            r.cbBytes == 256 && r.rtFormat == DXGI_FORMAT_R11G11B10_FLOAT;
        if (fingerprint)
        {
            // Most recently seen fingerprint wins (a recreated resolve shader
            // gets a new key; the stale one stops being seen).
            const auto seen = static_cast<std::uint32_t>(r.lastPresent.load(std::memory_order_relaxed));
            if (!best || seen >= bestPresent)
            {
                best = r.key.load(std::memory_order_relaxed);
                bestPresent = seen;
            }
            continue;
        }
        if (!fallback)
            fallback = r.key.load(std::memory_order_relaxed);
    }
    if (best)
    {
        g_fingerprintSeen.store(true, std::memory_order_relaxed);
        return best;
    }
    return fallback;
}

void OnImmediateWork(ID3D11DeviceContext* context)
{
    if (g_enabled && context)
        GpuStamp(context);
}

void OnPresentGpu(IDXGISwapChain* swapChain)
{
    if (!g_initDone)
        Init();
    if (!g_enabled || g_gpuFailed || !swapChain)
        return;
    if (!g_gpuArmed)
    {
        if (!GpuArm(swapChain))
        {
            g_gpuFailed = true;
            Logf("[GPUT] could not create timestamp queries; GPU timer off (no effect on play).");
            return;
        }
        g_gpuArmed = true;
    }
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(
        g_gpuImmediate.load(std::memory_order_acquire));
    if (!context)
        return;

    // Close the frame that is presenting now.
    const int current = g_gpuCurrent.load(std::memory_order_acquire);
    if (current >= 0)
    {
        auto& slot = g_gpu[current];
        if (slot.open)
        {
            const bool started = slot.startIssued.load(std::memory_order_acquire);
            if (started)
                context->End(slot.end);
            context->End(slot.disjoint);
            slot.open = false;
            slot.pending = started;
            if (!started)
            {
                ++g_gpuNoStart;
                g_gpuHaveLastEnd = false;
            }
        }
    }

    // Harvest finished frames, oldest first; never block (DONOTFLUSH).
    for (int k = 1; k <= kGpuSlots; ++k)
    {
        const int index = ((current < 0 ? 0 : current) + k) % kGpuSlots;
        auto& slot = g_gpu[index];
        if (!slot.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 start = 0, end = 0;
        if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint),
                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(slot.start, &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(slot.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            break;   // not ready; later frames will not be either
        slot.pending = false;
        if (disjoint.Disjoint || disjoint.Frequency == 0 || end < start)
        {
            ++g_gpuDisjoint;
            g_gpuHaveLastEnd = false;
            continue;
        }
        const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
        if (g_gpuSpanCount < kGpuWindow)
            g_gpuSpan[g_gpuSpanCount++] = static_cast<float>(static_cast<double>(end - start) * toMs);
        if (g_gpuHaveLastEnd && end > g_gpuLastEnd && g_gpuPeriodCount < kGpuWindow)
            g_gpuPeriod[g_gpuPeriodCount++] =
                static_cast<float>(static_cast<double>(end - g_gpuLastEnd) * toMs);
        g_gpuLastEnd = end;
        g_gpuHaveLastEnd = true;
        const UINT width = slot.renderWidth;
        if (width)
        {
            if (!g_gpuWindowWidthMin || width < g_gpuWindowWidthMin) g_gpuWindowWidthMin = width;
            if (width > g_gpuWindowWidthMax) g_gpuWindowWidthMax = width;
        }
    }

    // Open the next frame.
    const int next = (current + 1 + kGpuSlots) % kGpuSlots;
    auto& slot = g_gpu[next];
    if (slot.pending)
    {
        slot.pending = false;   // its results never arrived; drop rather than wait
        ++g_gpuStale;
        g_gpuHaveLastEnd = false;
    }
    context->Begin(slot.disjoint);
    slot.open = true;
    slot.startIssued.store(false, std::memory_order_relaxed);
    slot.renderWidth = g_lastVelocityWidth.load(std::memory_order_relaxed);
    g_gpuCurrent.store(next, std::memory_order_release);
    g_gpuStartIssued.store(false, std::memory_order_release);

    if (++g_gpuWindowPresents >= kGpuWindow)
        GpuReport();
}

void OnPass(const char* name)
{
    if (!g_initDone)
        Init();
    if (!g_enabled || !name || !*name)
        return;
    std::strncpy(g_currentPass, name, kPassNameChars - 1);
    g_currentPass[kPassNameChars - 1] = '\0';
    const auto count = g_passCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < kPassNames && i < count; ++i)
    {
        auto& p = g_pass[i];
        if (p.ready.load(std::memory_order_acquire) == 2 && std::strcmp(p.name, name) == 0)
        {
            p.announcements.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    const auto slot = g_passCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kPassNames)
    {
        g_passCount.store(kPassNames, std::memory_order_release);
        g_passOverflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& p = g_pass[slot];
    std::strncpy(p.name, name, kPassNameChars - 1);
    p.announcements.fetch_add(1, std::memory_order_relaxed);
    p.ready.store(2, std::memory_order_release);
}

void OnSetRenderTargets(UINT count, ID3D11RenderTargetView* const* views,
    ID3D11DepthStencilView* dsv)
{
    if (!g_initDone)
        Init();
    if (!g_enabled)
        return;
    if (views && count <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
    {
        for (UINT i = 0; i < count; ++i)
        {
            if (!views[i])
                continue;
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            views[i]->GetDesc(&desc);
            if (desc.Format != DXGI_FORMAT_R16G16_FLOAT)
                continue;
            UINT width = 0, height = 0;
            const auto key = ViewResource(views[i], &width, &height, true);
            if (!key || width < kFullSizeMinWidth)
                continue;
            g_lastVelocityWidth.store(width, std::memory_order_relaxed);
            if (Tracked* t = Track(g_velocityRes, key, width, height, desc.Format))
            {
                t->mrtSlots.fetch_or(1u << i, std::memory_order_relaxed);
                t->binds.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (dsv)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        dsv->GetDesc(&desc);
        if (IsDepthDsvFormat(desc.Format))
        {
            UINT width = 0, height = 0;
            const auto key = ViewResource(dsv, &width, &height, true);
            if (key && width >= kFullSizeMinWidth)
                if (Tracked* t = Track(g_depthRes, key, width, height, desc.Format))
                    t->binds.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void OnDraw(ID3D11DeviceContext* context, unsigned count, bool indexed)
{
    (void)indexed;
    if (!g_initDone)
        Init();
    if (!g_enabled || !context)
        return;
    GpuStamp(context);
    // Full-screen passes only; geometry never reaches a D3D call here.
    if (count == 0 || count > kMaxFullscreenCount)
        return;
    g_fullscreenThisFrame.fetch_add(1, std::memory_order_relaxed);
    // Nothing to match against until the engine has written velocity.
    if (g_velocityRes[0].ready.load(std::memory_order_acquire) != 2)
        return;
    if (!InspectThisPresent())
        return;
    Inspect(context, false);
}

void OnDispatch(ID3D11DeviceContext* context)
{
    if (!g_initDone)
        Init();
    if (!g_enabled || !context)
        return;
    GpuStamp(context);
    g_dispatchThisFrame.fetch_add(1, std::memory_order_relaxed);
    if (g_velocityRes[0].ready.load(std::memory_order_acquire) != 2)
        return;
    if (!InspectThisPresent())
        return;
    Inspect(context, true);
}

void OnMapped(ID3D11Resource* resource, D3D11_MAP type, void* data)
{
    if (!g_enabled || !resource || !data)
        return;
    if (type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE &&
        type != D3D11_MAP_WRITE)
        return;
    if (reinterpret_cast<std::uintptr_t>(resource) != g_watchCb.load(std::memory_order_relaxed))
        return;
    g_mapped.store(reinterpret_cast<std::uintptr_t>(data), std::memory_order_release);
}

void OnUnmapping(ID3D11Resource* resource)
{
    if (!g_enabled || !resource)
        return;
    if (reinterpret_cast<std::uintptr_t>(resource) != g_watchCb.load(std::memory_order_relaxed))
        return;
    const auto address = g_mapped.exchange(0, std::memory_order_acq_rel);
    if (!address)
        return;
    if (g_jitterSamples.load(std::memory_order_relaxed) >= kMaxJitterSamples)
        return;
    // Eight bytes out of write-combined memory, before the engine's Unmap.
    const float* floats = reinterpret_cast<const float*>(address);
    const float x = floats[8];
    const float y = floats[9];
    g_jitterLastX.store(FloatToBits(x), std::memory_order_relaxed);
    g_jitterLastY.store(FloatToBits(y), std::memory_order_relaxed);
    if (!g_jitterSeen)
    {
        g_jitterSeen = true;
        g_jitterMinX = g_jitterMaxX = x;
        g_jitterMinY = g_jitterMaxY = y;
    }
    else
    {
        if (x < g_jitterMinX) g_jitterMinX = x;
        if (x > g_jitterMaxX) g_jitterMaxX = x;
        if (y < g_jitterMinY) g_jitterMinY = y;
        if (y > g_jitterMaxY) g_jitterMaxY = y;
    }
    g_jitterSamples.fetch_add(1, std::memory_order_relaxed);
}

void OnPresent(bool aerEnabled, bool nativeStereo, int renderEye)
{
    if (!g_initDone)
        Init();
    if (!g_enabled)
        return;
    g_mode.store(nativeStereo ? 2 : (aerEnabled ? 1 : 0), std::memory_order_relaxed);
    g_renderEye.store(renderEye == 1 ? 1 : 0, std::memory_order_relaxed);
    const auto present = g_present.fetch_add(1, std::memory_order_relaxed) + 1;

    g_fullscreenPerFrame.store(g_fullscreenThisFrame.exchange(0, std::memory_order_relaxed),
        std::memory_order_relaxed);
    g_dispatchPerFrame.store(g_dispatchThisFrame.exchange(0, std::memory_order_relaxed),
        std::memory_order_relaxed);
    for (auto& r : g_reader)
    {
        const auto frameHits = r.hitsThisFrame.exchange(0, std::memory_order_relaxed);
        if (frameHits > r.peakPerFrame.load(std::memory_order_relaxed))
            r.peakPerFrame.store(frameHits, std::memory_order_relaxed);
    }

    const bool first = present == kFirstReportPresent;
    const bool periodic = present > kFirstReportPresent &&
        present - g_lastReport.load(std::memory_order_relaxed) >= kReportEveryPresents;
    if (!first && !periodic)
        return;
    g_lastReport.store(present, std::memory_order_relaxed);
    Report();
}

} // namespace DlssProbe
