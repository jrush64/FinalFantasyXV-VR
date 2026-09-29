#include "dlss_upscaler.h"
#include "dlss_probe.h"
#include "mv_fix.h"
#include "runtime_log.h"
namespace RetailXr { bool GetSplitSwapEyes(); }   // [PAIRBURST] headset side

#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_d3d.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

// The NGX loader reads its driver path from the registry; the hotkey needs user32.
// Declared here so every build that links this file (the smoke harness too) gets them.
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

// [DLSS] See dlss_upscaler.h.  Every D3D/NGX failure logs once and turns the
// module off for the session; the engine's own TAA then runs untouched.
namespace
{
constexpr int kFeatures = 4;           // one per eye family (depth buffer)
constexpr int kCaptures = 64;          // recent cbTemporalAA-shaped writes, any buffer
constexpr std::uint32_t kReportEvery = 600;
constexpr unsigned kMaxFullscreenCount = 6;
// c0..c6 of cbTemporalAA: screen size, frame bits, UV jitter, motion matrix.
constexpr int kShadowFloats = 28;
constexpr int kFamilies = 4;
constexpr int kTraceLines = 300;
constexpr char kProjectId[] = "5c9c7a2e-3f4b-4d1e-9a6b-2f0e8d4c1b7a";
std::atomic_bool g_aerBypass{false};

void Logf(const char* format, ...)
{
    char line[1400];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    RetailLogLine(line);
}

// Motion vectors for DLSS: pixel units, pointing from the current pixel to its
// previous position (prev - cur), which is exactly the engine's convention
// scaled by the target size.  Sentinel pixels (0, 1.0) carry no written
// motion; they are rebuilt from depth through the engine's own motion matrix,
// the same reconstruction the engine's TAA performs.  Depth is copied to a
// plain R32F texture so NGX never has to view the engine's R32G8X24 buffer.
constexpr char kPrepareShader[] = R"(
cbuffer TemporalAA : register(b0)
{
    float4 screenSize;
    float4 frameBits;
    float4 uvJitter;
    float4x4 motion;
};
// [MVFIX] v6.  Measured from raw dumps: the view
// constants' "previous" camera for an eye is the SAME frame's camera shifted
// 0.098 sideways, so the engine's TAA matrix and its written velocities hold a
// fixed fake parallax and NO head motion (engine 142-285 px of fake motion vs
// 34 px of real camera motion per frame).  ownMotion = the real camera motion
// from this eye's own view history; engineCam = the engine's camera part
// (same layout as the TAA matrix: x_row(u, v, depth, 1) -> previous uv).
cbuffer MotionFix : register(b1)
{
    float4x4 ownMotion;
    float4x4 engineCam;
    float4x4 unusedMatrix;
    float4 fixFlags;
};
Texture2D<float2> velocityIn : register(t0);
Texture2D<float> depthIn : register(t1);
RWTexture2D<float2> motionOut : register(u0);
RWTexture2D<float> depthOut : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    motionOut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    const float depth = depthIn[id.xy];
    depthOut[id.xy] = depth;
    const float2 size = float2(width, height);
    float2 v = velocityIn[id.xy];
    const bool sentinel = v.y == 1.0f;
    const float2 uv = (float2(id.xy) + 0.5f) / size;
    const float4 x = float4(uv, depth, 1.0f);
    if (sentinel)
    {
        float4 previous = fixFlags.x > 0.5f ? mul(ownMotion, x) : mul(motion, x);
        previous.xy /= previous.w;
        v = previous.xy - uv;
    }
    else if (fixFlags.x > 0.5f)
    {
        const float4 pt = mul(ownMotion, x);
        const float4 pe = mul(engineCam, x);
        const float2 corrected = v + (pt.xy / pt.w - pe.xy / pe.w);
        if (all(isfinite(corrected)))
            v = corrected;
    }
    motionOut[id.xy] = v * size;
}
)";

struct Feature
{
    std::uintptr_t depthKey{};         // identity of the eye family's depth buffer
    NVSDK_NGX_Handle* handle{};
    UINT width{}, height{};
    DXGI_FORMAT format{};
    ID3D11Texture2D* output{};
    ID3D11Texture2D* motion{};
    ID3D11UnorderedAccessView* motionUav{};
    ID3D11Texture2D* depth{};
    ID3D11UnorderedAccessView* depthUav{};
    std::uint32_t lastPresent{};
    std::uint64_t evaluations{};
    // [DLSSTILE] A picture NGX refuses whole is resolved as side-by-side
    // columns, each its own feature with its own history.  Columns overlap by
    // kTileOverlap on each side; only the core of each is kept.
    int tiles{1};
    UINT tileWidth{};
    NVSDK_NGX_Handle* tileHandle[4]{};
    ID3D11Texture2D* tileColour{};
    ID3D11Texture2D* tileMotion{};
    ID3D11Texture2D* tileDepth{};
    ID3D11Texture2D* tileOutput{};
};
Feature g_feature[kFeatures];
constexpr int kMaxTiles = 4;
constexpr UINT kTileOverlap = 64;
// The column count that worked for a size, so a recreated feature does not
// retry the counts NGX already refused.
UINT g_tileHintWidth = 0, g_tileHintHeight = 0;
int g_tileHint = 1;

// cbTemporalAA writes, recognised by CONTENT, not by buffer: the engine draws
// its per-draw constants from a large pool of 256-byte buffers, so the buffer
// the resolve binds changes from frame to frame (v1 of this module learnt 8
// buffers, saw 39 frames of DLSS at startup and then skipped every frame).
// A capture is the first 48 bytes of a 256-byte constant-buffer write whose
// c0 is (W, H, 1/W, 1/H); at the resolve draw the newest unconsumed capture
// from the buffer actually bound is used, once.
struct Capture
{
    std::uintptr_t resource{};
    std::uint64_t seq{};
    bool consumed{};
    float data[kShadowFloats]{};
};
Capture g_capture[kCaptures];
std::uint64_t g_captureSeq = 0;
SRWLOCK g_captureLock = SRWLOCK_INIT;
thread_local std::uintptr_t t_mappedResource = 0;
thread_local const void* t_mappedData = nullptr;

bool g_initDone = false;
bool g_active = true;               // always on while the engine runs TAA
bool g_failed = false;
// [DLSSSIZE] A size NGX refused.  Only that size is parked (the engine's own
// resolve runs meanwhile); a different render size is tried afresh.
UINT g_refusedWidth = 0, g_refusedHeight = 0;
bool g_ngxReady = false;
bool g_loggedDeferred = false;
bool g_loggedShadow = false;
ID3D11Device* g_device{};           // identity; the game's device outlives this module
ID3D11DeviceContext* g_immediate{}; // identity
NVSDK_NGX_Parameter* g_params{};
ID3D11ComputeShader* g_prepare{};
wchar_t g_dllDir[MAX_PATH]{};
std::uint32_t g_present = 0;
std::uint64_t g_evaluations = 0;
std::uint64_t g_skipNoShadow = 0, g_skipBadInputs = 0, g_skipDeferred = 0;
std::uint64_t g_captures = 0;
// Diagnostic marker (green square in the DLSS eye); nothing arms it any more.
std::uint32_t g_markerUntil = 0;

// ---- second eye ----------------------------------------------------------
// An eye family = one depth buffer + the velocity buffer rendered with it.
// References are held so views can be made on demand; at most kFamilies
// (stereo toggles recreate them; the least recently seen is recycled).
struct Family
{
    std::uintptr_t depthKey{};
    ID3D11Resource* depth{};
    ID3D11Resource* velocity{};
    ID3D11ShaderResourceView* depthSrv{};
    ID3D11ShaderResourceView* velocitySrv{};
    std::uint32_t lastSeen{};
};
Family g_family[kFamilies];
SRWLOCK g_familyLock = SRWLOCK_INIT;
std::uintptr_t g_sceneColour = 0;       // shared scene colour (the resolve's slot 0)
std::uintptr_t g_resolvedDepth = 0;     // family the engine resolves itself
std::uintptr_t g_currentScene = 0;      // immediate ctx: family whose scene is drawing
std::uintptr_t g_anchorPs = 0;          // first scene-colour reader after the resolve
bool g_learnAnchor = false;
// The unresolved eye renders FIRST in a frame (DLSSTRACE), before
// this frame's cbTemporalAA exists.  Its jitter is predicted from the resolved
// eye's own history: one real value per frame is recorded, the repeat length
// of the sequence is found, and the next value is the one that followed a
// period earlier.  (A Halton(2,3)-by-one-index model was tried first and never
// matched: x and y sit at different sequence positions.)  Every prediction is
// checked against the resolved eye's real value later in the same frame.  The
// motion matrix is the previous frame's (one frame stale).
constexpr int kJitterHistory = 160;
float g_jitterHistory[kJitterHistory][2]{};
int g_jitterCount = 0;                  // total values recorded
int g_jitterPeriod = 0;                 // 0 = not yet found
float g_predicted[2]{};
std::uint32_t g_predictedPresent = 0;
std::uint32_t g_lastJitterPresent = 0;
std::uint64_t g_predictHits = 0, g_predictMisses = 0, g_predictUnknown = 0;
bool g_loggedPeriod = false;
ID3D11RenderTargetView* g_colourRtv{};  // for the toggle marker in the second eye
// [GLAREFIX] Left eye blown out white/pink in TAA mode only
// (never in FXAA).  Retail strips the second output's post-effect
// banks, so in TAA mode some post inputs of the unresolved eye are EMPTY
// (null SRV -> reads 0 -> e.g. exposure from zero luminance -> blown out).
// Every post draw / compute dispatch in the unresolved eye is compared with the
// same shader in the resolved eye: where the unresolved eye has a null slot and
// the resolved eye had a real view, the resolved eye's view is bound.  Exposure,
// glare and similar inputs are view-independent enough to share.
std::uintptr_t g_resolveOutput = 0;     // the resolve's render target (TAA output)
// Last present in which the engine ran its resolve.  If it stops (AA switched
// to FXAA mid-session) everything learnt from it is dropped, so nothing here
// keeps acting on stale TAA-mode state.
std::uint32_t g_lastResolvePresent = 0;
bool g_staleReset = false;
// [DECALFILL] Compute passes only.  The left (unresolved) eye's
// DeferredDecal compute pass runs with slots 3-7 EMPTY (decal textures 256x256
// fmt34 / 512x512 fmt28 / 512x512 BC7, 1x1 defaults, 16000-B decal list) that
// the resolved eye binds -> the Regalia's wrap decal missing in one eye
// (black car left, white decal car right).  Decals are world data,
// identical for both eyes, so the resolved eye's views are copied into the
// left eye's empty compute slots.  The per-DRAW fill stays off.
bool g_fillComputeEmpty = false;   // off: filling alone does not restore the decal
std::uint64_t g_computeFills = 0;
bool g_glareFix = false;   // off: filling alone does not change the glare
bool g_numpad6Down = false;
constexpr int kBindShaders = 256;
constexpr int kBindSlots = 16;
struct ShaderBindings
{
    std::uintptr_t shader{};
    bool compute{};
    ID3D11ShaderResourceView* srv[kBindSlots]{};   // resolved eye's, one reference each
    std::uint32_t loggedSlots{};                    // slots already reported
};
ShaderBindings g_bind[kBindShaders];
std::uint64_t g_glareFills = 0, g_glareDraws = 0, g_bindOverflow = 0;
std::uintptr_t g_colourRtvKey = 0;
bool g_secondDone = false;
float g_lastParams[32]{};               // c0..c6 + pad, for the module's own constant buffer
bool g_haveParams = false;
std::uint32_t g_paramsPresent = 0;
ID3D11Buffer* g_ownCb{};
std::uint64_t g_secondEvaluations = 0, g_secondSameFrameParams = 0;
std::uint64_t g_secondNoFamily = 0, g_secondNoColour = 0, g_secondNoParams = 0,
    g_secondNoPrediction = 0,
    g_secondBadSize = 0;
std::uint32_t g_sceneSwitchesThisPresent = 0;
bool g_loggedAnchor = false, g_loggedNoColour = false, g_loggedSecond = false;
// One-shot event trace of a stereo frame, so a misplaced second-eye pass is
// visible in the log instead of needing another round trip.
int g_traceState = 0;                   // 0 idle, 1 armed, 2 recording, 3 done
int g_traceLines = 0;

void Trace(const char* format, ...)
{
    if (g_traceState != 2 || g_traceLines >= kTraceLines)
        return;
    ++g_traceLines;
    char line[512];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    Logf("[DLSSTRACE] %s", line);
}

const char* FamilyTag(std::uintptr_t depthKey)
{
    if (!depthKey) return "none";
    return depthKey == g_resolvedDepth ? "RESOLVED-EYE" : "SECOND-EYE";
}

const float* JitterAt(int index)   // index into the full recorded sequence
{
    return g_jitterHistory[index % kJitterHistory];
}


// ---- [MVFIX] per eye family cameras ---------------------------------------
// Per eye family and per VS constant slot, a short history of DISTINCT view
// uploads (TAA jitter makes every frame's view different; the geometry passes
// are recorded on worker threads, so present numbers cannot pair them).  The
// newest two entries give the eye's real camera motion (see MotionFix below).
struct CamState
{
    float view[16]{}, proj[16]{}, enginePrevView[16]{}, enginePrevProj[16]{};
    std::int64_t qpc{};   // [LEFTDUP] capture time
};
// [LEFTDUP] The LEFT (second, in-place DLSS) eye's pass uploads the
// same view twice per frame with different TAA jitter; the v6 rule records
// both, so every other pair is "this frame vs itself" = zero camera motion ->
// motion that stutters on/off in the left eye only.
// For the
// second-eye family ONLY, a same-view upload within kLeftDupWindowMs of the
// previous one refreshes that entry instead of adding one.  The resolved
// (right) eye keeps the v6 rule untouched.
constexpr double kLeftDupWindowMs = 4.0;
std::uint64_t g_leftDupMerged{}, g_leftSameViewSlow{};
constexpr int kRing = 4, kCamSlots = 4;
struct SlotRing { CamState e[kRing]; std::uint64_t count{}; };
struct FamilyCam
{
    std::uintptr_t key{};
    SlotRing ring[kCamSlots];
    std::uint64_t captures{}, fixed{};
};
float g_lastMotion[16]{};   // TAA matrix of the resolve being evaluated, as bound (column-major)
bool g_motionFresh = false;
FamilyCam g_cams[kFeatures];
SRWLOCK g_camLock = SRWLOCK_INIT;
thread_local std::uintptr_t t_pendingFamily = 0;
thread_local int t_pendingBudget = 0;
ID3D11Buffer* g_motionCb{};
std::uint64_t g_camMisses{};
// [CUTRESET] a lens change or a camera jump between two consecutive views
// of an eye: the engine's own motion is meaningless for that frame, so
// instead of falling back to it (smear) the eye's DLSS history is reset.
bool g_cutRequested = false;
// Both OFF: together they reset DLSS histories too often (dozens of resets
// in a few minutes).  Off = the v6 motion logic.
constexpr bool kCutReset = false;
constexpr bool kSameViewDedupe = false;
std::uint64_t g_cutsLens{}, g_cutsTurn{}, g_cutsJump{};
double g_lastOwnShift[2]{};   // [0] resolved eye, [1] second eye: centre shift px, for consistency
std::uint64_t g_eyeMismatch{};
// [PAIRBURST] per-frame pairing detail: 40 presents after a user mark, plus
// the first 40 eye-mismatch frames.  Tells whether "last" is really the
// previous frame of the same eye (dRight ~ 0) or the other eye of the same
// frame (dRight ~ +-baseline, identical forward).
std::uint32_t g_burstUntil = 0;
float g_lastEyePos[2][3]{};   // [0] resolved eye, [1] second eye: latest camera position
bool g_haveEyePos[2]{};
std::uint64_t g_mismatchLogged = 0;
int g_camSlotLogged = -1;

void MatMul(const float* a, const float* b, float* o)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            o[i * 4 + j] = s;
        }
}
bool MatInverse(const float* m, float* inv)
{
    float t[16];
    t[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    t[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    t[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    t[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    t[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    t[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    t[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    t[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    t[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    t[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    t[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    t[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    t[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    t[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    t[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    t[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const float det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    if (!(std::fabs(det) > 1e-20f) || !std::isfinite(det)) return false;
    for (int i = 0; i < 16; ++i) inv[i] = t[i] / det;
    return true;
}
// View * Projection with the TAA jitter (projection row 2, columns 0/1) removed.
void ViewProjNoJitter(const float* view, const float* proj, float* out)
{
    float p[16];
    std::memcpy(p, proj, sizeof(p));
    p[8] = 0.0f;
    p[9] = 0.0f;
    MatMul(view, p, out);
}

FamilyCam* CamFor(std::uintptr_t key, bool create)
{
    for (auto& c : g_cams) if (c.key == key) return &c;
    if (!create) return nullptr;
    for (auto& c : g_cams) if (!c.key) { c = FamilyCam{}; c.key = key; return &c; }
    return nullptr;
}

const CamState* RingAt(const SlotRing& r, int back)
{
    if (r.count < static_cast<std::uint64_t>(back) + 1) return nullptr;
    return &r.e[(r.count - 1 - back) % kRing];
}

void StoreFamilyCam(std::uintptr_t key, const float* f, int slot)
{
    if (slot < 0 || slot >= kCamSlots) return;
    AcquireSRWLockExclusive(&g_camLock);
    FamilyCam* c = CamFor(key, true);
    if (c)
    {
        SlotRing& r = c->ring[slot];
        const CamState* latest = RingAt(r, 0);
        // [SAMEVIEW] A new history entry only when the CAMERA
        // (view matrix) changed.  The left eye's pass uploads its view twice
        // per frame with different TAA jitter; keying on view + projection
        // made those two a "pair", so the left eye got zero camera motion
        // while the head moved (left-eye DLSS smear:
        // ring +2 per present, own=0.0px).  Same view = same frame: refresh
        // the entry in place.
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const bool secondEye = key != g_resolvedDepth;
        const bool sameView = latest && std::memcmp(latest->view, f + 16, 64) == 0;
        const double gapMs = latest && freq.QuadPart
            ? static_cast<double>(now.QuadPart - latest->qpc) * 1000.0 / static_cast<double>(freq.QuadPart) : 1e9;
        if (secondEye && sameView)
            (gapMs < kLeftDupWindowMs ? g_leftDupMerged : g_leftSameViewSlow)++;
        if ((kSameViewDedupe || (secondEye && gapMs < kLeftDupWindowMs)) && sameView)
        {
            CamState& e = r.e[(r.count - 1) % kRing];
            std::memcpy(e.proj, f, 64);
            std::memcpy(e.enginePrevView, f + 88, 64);
            std::memcpy(e.enginePrevProj, f + 104, 64);
        }
        else if (!latest || std::memcmp(latest->view, f + 16, 64) != 0 || std::memcmp(latest->proj, f, 64) != 0)
        {
            // v6 rule: new entry when view OR projection changed.
            CamState& e = r.e[r.count % kRing];
            e.qpc = now.QuadPart;
            std::memcpy(e.proj, f, 64);
            std::memcpy(e.view, f + 16, 64);
            std::memcpy(e.enginePrevView, f + 88, 64);
            std::memcpy(e.enginePrevProj, f + 104, 64);
            ++r.count;
            ++c->captures;
        }
    }
    ReleaseSRWLockExclusive(&g_camLock);
}

void CamViewProj(const float* view, const float* proj, bool jitter, float* out)
{
    if (jitter) MatMul(view, proj, out);
    else ViewProjNoJitter(view, proj, out);
}

// Camera motion in the TAA matrix's layout: x_row(u, v, depth, 1) * R gives
// the homogeneous previous uv.  R = A' * inv(VPcur) * VPprev * B' (row
// vectors), which bound as a column-major float4x4 is exactly R row-major.
bool OwnMotion(const CamState& cur, const CamState& last, bool jitter, float* r)
{
    float vpc[16], vpl[16], inv[16];
    CamViewProj(cur.view, cur.proj, jitter, vpc);
    CamViewProj(last.view, last.proj, jitter, vpl);
    if (!MatInverse(vpc, inv)) return false;
    const float at[16]{2, 0, 0, 0,  0, -2, 0, 0,  0, 0, 1, 0,  -1, 1, 0, 1};
    const float bt[16]{0.5f, 0, 0, 0,  0, -0.5f, 0, 0,  0, 0, 1, 0,  0.5f, 0.5f, 0, 1};
    float t1[16], t2[16];
    MatMul(at, inv, t1);
    MatMul(t1, vpl, t2);
    MatMul(t2, bt, r);
    for (int i = 0; i < 16; ++i) if (!std::isfinite(r[i])) return false;
    return true;
}

bool ApplyMotion(const float* m, float u, float v, float d, float* out)
{
    const float in[4]{u, v, d, 1.0f};
    float p[4]{};
    for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) p[r] += m[k * 4 + r] * in[k];
    if (!(std::fabs(p[3]) > 1e-12f)) return false;
    out[0] = p[0] / p[3];
    out[1] = p[1] / p[3];
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}

// Mean distance in px between two motion matrices over a 3x3 grid x 4 depths.
double MotionDiffPx(const float* a, const float* b, float w, float h, double* worst)
{
    const float coords[3]{0.2f, 0.5f, 0.8f};
    const float depths[4]{0.001f, 0.01f, 0.1f, 0.5f};
    double sum = 0.0, mx = 0.0;
    int n = 0;
    for (float u : coords) for (float v : coords) for (float d : depths)
    {
        float pa[2], pb[2];
        if (!ApplyMotion(a, u, v, d, pa) || !ApplyMotion(b, u, v, d, pb)) continue;
        const double e = std::hypot((pa[0] - pb[0]) * w, (pa[1] - pb[1]) * h);
        if (!std::isfinite(e)) continue;
        sum += e; if (e > mx) mx = e; ++n;
    }
    if (worst) *worst = mx;
    return n ? sum / n : 1e9;
}

// ---- [MVFIX] v6 double-precision camera motion ------------------------------
void DMul(const double* a, const double* b, double* o)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            double t = 0.0;
            for (int k = 0; k < 4; ++k) t += a[i * 4 + k] * b[k * 4 + j];
            o[i * 4 + j] = t;
        }
}
bool DInverse(const double* m, double* out)
{
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c)
    {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (!(std::fabs(a[piv][c]) > 1e-300)) return false;
        if (piv != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[piv][j]);
        const double pv = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= pv;
        for (int r = 0; r < 4; ++r)
            if (r != c)
            {
                const double f = a[r][c];
                if (f != 0.0) for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][4 + j];
    return true;
}
// x_row(u, v, depth, 1) * R = homogeneous previous uv, for a pixel rendered
// with (view, proj) whose previous position is given by (prevView, prevProj).
// Bound as a column-major float4x4 this is exactly R row-major.
bool CameraMotion(const float* view, const float* proj, const float* prevView, const float* prevProj,
    bool noJitter, float* out)
{
    double v[16], pr[16], pv[16], pp[16];
    for (int i = 0; i < 16; ++i) { v[i] = view[i]; pr[i] = proj[i]; pv[i] = prevView[i]; pp[i] = prevProj[i]; }
    if (noJitter) { pr[8] = pr[9] = 0.0; pp[8] = pp[9] = 0.0; }
    double vpc[16], vpp[16], inv[16], t1[16], t2[16], r[16];
    DMul(v, pr, vpc);
    DMul(pv, pp, vpp);
    if (!DInverse(vpc, inv)) return false;
    const double at[16]{2, 0, 0, 0,  0, -2, 0, 0,  0, 0, 1, 0,  -1, 1, 0, 1};
    const double bt[16]{0.5, 0, 0, 0,  0, -0.5, 0, 0,  0, 0, 1, 0,  0.5, 0.5, 0, 1};
    DMul(at, inv, t1);
    DMul(t1, vpp, t2);
    DMul(t2, bt, r);
    for (int i = 0; i < 16; ++i)
    {
        if (!std::isfinite(r[i])) return false;
        out[i] = static_cast<float>(r[i]);
    }
    return true;
}
double CentreShiftPx(const float* m, float depth, float width, float height)
{
    float q[2];
    if (!ApplyMotion(m, 0.5f, 0.5f, depth, q)) return 1e9;
    return std::hypot((q[0] - 0.5f) * width, (q[1] - 0.5f) * height);
}

// Fills the MotionFix constant buffer for this family; false = no correction.
bool BuildMotionFix(std::uintptr_t key, float* cb, float width, float height)
{
    std::memset(cb, 0, sizeof(float) * 52);
    const bool fresh = g_motionFresh;
    g_motionFresh = false;
    if (!MvFix::g_dlssFix.load(std::memory_order_relaxed))
        return false;
    const bool resolved = key == g_resolvedDepth;
    FamilyCam snap;
    bool have = false;
    AcquireSRWLockShared(&g_camLock);
    for (auto& c : g_cams)
        if (c.key == key) { snap = c; have = true; break; }
    ReleaseSRWLockShared(&g_camLock);
    const SlotRing* ring = nullptr;
    if (have)
        ring = snap.ring[0].count >= 2 ? &snap.ring[0] : (snap.ring[1].count >= 2 ? &snap.ring[1] : nullptr);
    const CamState* cur = ring ? RingAt(*ring, 0) : nullptr;
    const CamState* last = ring ? RingAt(*ring, 1) : nullptr;
    if (!cur || !last)
    {
        ++g_camMisses;
        return false;
    }
    // Same kind of camera (lens) and a plausible single-frame turn.
    const bool sameLens = std::fabs(cur->proj[0] - last->proj[0]) < 1e-3f && std::fabs(cur->proj[5] - last->proj[5]) < 1e-3f;
    const float forwardDot = cur->view[2] * last->view[2] + cur->view[6] * last->view[6] + cur->view[10] * last->view[10];
    float own[16], engine[16];
    if (!sameLens) { ++g_cutsLens; g_cutRequested = true; return false; }          // [CUTRESET]
    if (!(forwardDot > 0.95f)) { ++g_cutsTurn; g_cutRequested = true; return false; }
    if (!CameraMotion(cur->view, cur->proj, last->view, last->proj, true, own) ||
        !CameraMotion(cur->view, cur->proj, cur->enginePrevView, cur->enginePrevProj, false, engine))
    {
        ++g_camMisses;
        return false;
    }
    const double ownShift = CentreShiftPx(own, 0.99f, width, height);
    if (!(ownShift < 400.0)) { ++g_cutsJump; g_cutRequested = true; return false; }
    g_lastOwnShift[resolved ? 0 : 1] = ownShift;
    bool mismatch = false;
    if (!resolved && g_lastOwnShift[0] > 6.0 && ownShift > 6.0)
    {
        // Both eyes moved this frame; they should agree (same head, same camera).
        const double ratio = ownShift / g_lastOwnShift[0];
        if (ratio < 0.6 || ratio > 1.6)
        {
            ++g_eyeMismatch;
            mismatch = true;
        }
    }
    if (g_present < g_burstUntil || (mismatch && g_mismatchLogged < 40))
    {
        if (mismatch && g_present >= g_burstUntil) ++g_mismatchLogged;
        // Eye positions (row-vector world-to-view: eye = -(t . columns)).
        auto eyeOf = [](const float* V, float* e) {
            e[0] = -(V[12] * V[0] + V[13] * V[1] + V[14] * V[2]);
            e[1] = -(V[12] * V[4] + V[13] * V[5] + V[14] * V[6]);
            e[2] = -(V[12] * V[8] + V[13] * V[9] + V[14] * V[10]);
        };
        float ec[3], el[3];
        eyeOf(cur->view, ec);
        eyeOf(last->view, el);
        // Which eye this is: compare with the other family's latest position
        // along camera right.  Headset side follows Swap eyes.
        const int me = resolved ? 0 : 1;
        for (int k = 0; k < 3; ++k) g_lastEyePos[me][k] = ec[k];
        g_haveEyePos[me] = true;
        const char* side = "?";
        if (g_haveEyePos[1 - me])
        {
            const float* o = g_lastEyePos[1 - me];
            const float along = (ec[0] - o[0]) * cur->view[0] + (ec[1] - o[1]) * cur->view[4] + (ec[2] - o[2]) * cur->view[8];
            const bool gameRight = along > 0.0f;
            const bool headsetRight = gameRight != RetailXr::GetSplitSwapEyes();
            side = headsetRight ? "RIGHT eye" : "LEFT eye";
        }
        const float d[3]{ec[0] - el[0], ec[1] - el[1], ec[2] - el[2]};
        const float* V = cur->view;
        const float dRight = d[0] * V[0] + d[1] * V[4] + d[2] * V[8];
        const float dUp = d[0] * V[1] + d[1] * V[5] + d[2] * V[9];
        const float dFwd = d[0] * V[2] + d[1] * V[6] + d[2] * V[10];
        Logf("[DLSSMV] pair %s (%s) present=%u ring=%llu own=%.1fpx other=%.1fpx fwdDot=%.6f dRight=%+.4f dUp=%+.4f dFwd=%+.4f eye=(%.2f,%.2f,%.2f)%s",
            side, FamilyTag(key), g_present, static_cast<unsigned long long>(ring->count), ownShift,
            g_lastOwnShift[resolved ? 1 : 0], forwardDot, dRight, dUp, dFwd, ec[0], ec[1], ec[2],
            mismatch ? " MISMATCH" : "");
    }
    std::memcpy(cb, own, 64);
    std::memcpy(cb + 16, engine, 64);
    cb[48] = 1.0f;
    FamilyCam* live = nullptr;
    for (auto& c : g_cams) if (c.key == key) live = &c;
    const std::uint64_t fixed = live ? ++live->fixed : 0;
    if ((fixed % 600) == 1)
    {
        double check = -1.0;
        if (resolved && fresh)
            check = MotionDiffPx(engine, g_lastMotion, width, height, nullptr);
        Logf("[DLSSMV] v6 %s: real camera motion %.1f px (centre, far) replaces the engine's %.1f px; engine-model check vs TAA matrix %.2f px (captures=%llu fixed=%llu misses=%llu cuts lens/turn/jump=%llu/%llu/%llu eye-mismatch=%llu left-dup merged/slow=%llu/%llu)",
            FamilyTag(key), ownShift, CentreShiftPx(engine, 0.99f, width, height), check,
            static_cast<unsigned long long>(snap.captures), static_cast<unsigned long long>(fixed),
            static_cast<unsigned long long>(g_camMisses), static_cast<unsigned long long>(g_cutsLens),
            static_cast<unsigned long long>(g_cutsTurn), static_cast<unsigned long long>(g_cutsJump),
            static_cast<unsigned long long>(g_eyeMismatch),
            static_cast<unsigned long long>(g_leftDupMerged), static_cast<unsigned long long>(g_leftSameViewSlow));
    }
    return true;
}

bool SameJitter(const float* a, const float* b)
{
    return std::fabs(a[0] - b[0]) < 1e-3f && std::fabs(a[1] - b[1]) < 1e-3f;
}

// Smallest period P (2..64) that the last 2P recorded values obey.
int FindPeriod()
{
    const int n = g_jitterCount;
    for (int period = 2; period <= 64; ++period)
    {
        if (n < 2 * period + 1 || 2 * period + 1 > kJitterHistory)
            break;
        bool ok = true;
        for (int k = 0; k < period + 1 && ok; ++k)
            ok = SameJitter(JitterAt(n - 1 - k), JitterAt(n - 1 - k - period));
        if (ok)
            return period;
    }
    return 0;
}

// Prediction for the frame after the last recorded one.
bool PredictJitter(float* out)
{
    if (!g_jitterPeriod || g_jitterCount < g_jitterPeriod)
        return false;
    const float* next = JitterAt(g_jitterCount - g_jitterPeriod);
    out[0] = next[0];
    out[1] = next[1];
    return true;
}

// The resolved eye's real jitter, once per frame: verify this frame's
// prediction, record, and (re)learn the period.
void LearnJitter(float jx, float jy)
{
    if (g_lastJitterPresent == g_present && g_jitterCount)
        return;
    g_lastJitterPresent = g_present;
    const float real[2] = {jx, jy};
    if (g_predictedPresent == g_present)
        (SameJitter(g_predicted, real) ? g_predictHits : g_predictMisses)++;
    float* slot = g_jitterHistory[g_jitterCount % kJitterHistory];
    slot[0] = jx;
    slot[1] = jy;
    ++g_jitterCount;
    const int period = FindPeriod();
    if (period && period != g_jitterPeriod)
    {
        g_jitterPeriod = period;
        if (!g_loggedPeriod)
        {
            g_loggedPeriod = true;
            Logf("[DLSS] jitter sequence repeats every %d frames; the second eye's jitter is now "
                 "predicted from it.", period);
        }
    }
}

ShaderBindings* BindingsFor(std::uintptr_t shader, bool compute, bool create)
{
    for (auto& b : g_bind)
    {
        if (b.shader == shader && b.compute == compute)
            return &b;
        if (!b.shader)
        {
            if (!create)
                return nullptr;
            b.shader = shader;
            b.compute = compute;
            return &b;
        }
    }
    ++g_bindOverflow;
    return nullptr;
}

void GetViews(ID3D11DeviceContext* c, bool compute, ID3D11ShaderResourceView** views)
{
    if (compute)
        c->CSGetShaderResources(0, kBindSlots, views);
    else
        c->PSGetShaderResources(0, kBindSlots, views);
}

void DescribeView(ID3D11ShaderResourceView* v, char* out, std::size_t size)
{
    out[0] = '\0';
    if (!v) { _snprintf_s(out, size, _TRUNCATE, "null"); return; }
    ID3D11Resource* r{};
    v->GetResource(&r);
    if (!r) { _snprintf_s(out, size, _TRUNCATE, "?"); return; }
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        _snprintf_s(out, size, _TRUNCATE, "tex %ux%u fmt %d", d.Width, d.Height, static_cast<int>(d.Format));
    }
    else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER)
    {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
        _snprintf_s(out, size, _TRUNCATE, "buffer %u B", d.ByteWidth);
    }
    else
        _snprintf_s(out, size, _TRUNCATE, "resource dim %d", static_cast<int>(dim));
    r->Release();
}

// Resolved eye: remember this shader's views.
void RecordBindings(ID3D11DeviceContext* c, std::uintptr_t shader, bool compute)
{
    ShaderBindings* b = BindingsFor(shader, compute, true);
    if (!b)
        return;
    ID3D11ShaderResourceView* views[kBindSlots]{};
    GetViews(c, compute, views);
    for (int i = 0; i < kBindSlots; ++i)
    {
        if (b->srv[i]) b->srv[i]->Release();
        b->srv[i] = views[i];          // keeps the reference GetViews took
    }
}

// Unresolved eye: fill empty slots from the resolved eye's views of the same shader.
void FillEmptyInputs(ID3D11DeviceContext* c, std::uintptr_t shader, bool compute)
{
    ShaderBindings* b = BindingsFor(shader, compute, false);
    if (!b)
        return;
    ID3D11ShaderResourceView* views[kBindSlots]{};
    GetViews(c, compute, views);
    bool filled = false;
    for (int i = 0; i < kBindSlots; ++i)
    {
        if (!views[i] && b->srv[i])
        {
            if (g_glareFix || (compute && g_fillComputeEmpty))
            {
                if (compute)
                    c->CSSetShaderResources(i, 1, &b->srv[i]);
                else
                    c->PSSetShaderResources(i, 1, &b->srv[i]);
                ++g_glareFills;
                if (compute) ++g_computeFills;
                filled = true;
            }
            if (!(b->loggedSlots & (1u << i)))
            {
                b->loggedSlots |= 1u << i;
                char desc[96];
                DescribeView(b->srv[i], desc, sizeof(desc));
                Logf("[GLAREFIX] %s %p slot %d: EMPTY in the unresolved (left) eye, the resolved "
                     "eye binds %s -> %s", compute ? "CS" : "PS", reinterpret_cast<void*>(shader), i,
                    desc, (g_glareFix || (compute && g_fillComputeEmpty)) ? "FILLED from the resolved eye" : "fix off, left empty");
            }
        }
        if (views[i]) views[i]->Release();
    }
    if (filled)
        ++g_glareDraws;
}

bool ReadsSceneColour(ID3D11DeviceContext* context)
{
    ID3D11ShaderResourceView* srv[16]{};
    context->PSGetShaderResources(0, 16, srv);
    bool reads = false;
    for (auto* v : srv)
    {
        if (!v) continue;
        ID3D11Resource* r{};
        v->GetResource(&r);
        if (r && reinterpret_cast<std::uintptr_t>(r) == g_sceneColour)
            reads = true;
        if (r) r->Release();
        v->Release();
    }
    return reads;
}
float g_lastJitterX = 0.0f, g_lastJitterY = 0.0f;

void Fail(const char* what, long long code)
{
    if (g_failed)
        return;
    g_failed = true;
    Logf("[DLSS] OFF for this session: %s (code 0x%llX). The engine's TAA keeps running.", what,
        static_cast<unsigned long long>(code));
}

void InitIni()
{
    if (g_initDone)
        return;
    g_initDone = true;
    HMODULE self{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&InitIni), &self) &&
        GetModuleFileNameW(self, g_dllDir, MAX_PATH))
    {
        if (wchar_t* slash = std::wcsrchr(g_dllDir, L'\\'))
            *(slash + 1) = L'\0';
        wchar_t ini[MAX_PATH]{};
        std::wcsncpy(ini, g_dllDir, MAX_PATH - 1);
        std::wcsncat(ini, L"ffxv-vr.ini", MAX_PATH - 1 - std::wcslen(ini));
    }
    g_active = true;
    Logf("[DLSS] DLSS 4 DLAA module ON: replaces the engine's temporal resolve whenever the game "
         "runs TAA; one NGX history per eye family.");
}

bool InitNgx(ID3D11DeviceContext* context)
{
    context->GetDevice(&g_device);
    if (!g_device)
    {
        Fail("no device on the resolve context", 0);
        return false;
    }
    g_device->GetImmediateContext(&g_immediate);
    // Identity only; the game owns both for the life of the process.
    g_immediate->Release();
    g_device->Release();

    const wchar_t* paths[] = {g_dllDir};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_dllDir, g_device, &info);
    if (NVSDK_NGX_FAILED(result))
    {
        Fail("NGX init failed (driver too old, or no RTX GPU)", result);
        return false;
    }

    NVSDK_NGX_Parameter* caps{};
    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(result) || !caps)
    {
        Fail("NGX capability query failed", result);
        return false;
    }
    int available = 0, needsDriver = 0;
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
        &needsDriver);
    NVSDK_NGX_D3D11_DestroyParameters(caps);
    if (!available)
    {
        Fail(needsDriver ? "DLSS needs a newer NVIDIA driver"
                         : "DLSS not available - is nvngx_dlss.dll beside ffxv_s.exe?",
            0);
        return false;
    }

    result = NVSDK_NGX_D3D11_AllocateParameters(&g_params);
    if (NVSDK_NGX_FAILED(result) || !g_params)
    {
        Fail("NGX parameter allocation failed", result);
        return false;
    }

    ID3DBlob* code{};
    ID3DBlob* errors{};
    const HRESULT hr = D3DCompile(kPrepareShader, sizeof(kPrepareShader) - 1, "dlss_prepare",
        nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || !code)
    {
        Logf("[DLSS] prepare shader: %s",
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        if (errors) errors->Release();
        Fail("motion-vector shader failed to compile", hr);
        return false;
    }
    if (errors) errors->Release();
    const HRESULT created = g_device->CreateComputeShader(code->GetBufferPointer(),
        code->GetBufferSize(), nullptr, &g_prepare);
    code->Release();
    if (FAILED(created))
    {
        Fail("motion-vector shader creation failed", created);
        return false;
    }
    g_ngxReady = true;
    Logf("[DLSS] NGX ready: DLSS Super Resolution available, preset K (transformer) in DLAA "
         "mode, flags HDR | MV at render res | auto exposure.");
    return true;
}

void ReleaseFeature(Feature& f)
{
    if (f.handle) NVSDK_NGX_D3D11_ReleaseFeature(f.handle);
    for (auto* h : f.tileHandle)
        if (h) NVSDK_NGX_D3D11_ReleaseFeature(h);
    if (f.tileColour) f.tileColour->Release();
    if (f.tileMotion) f.tileMotion->Release();
    if (f.tileDepth) f.tileDepth->Release();
    if (f.tileOutput) f.tileOutput->Release();
    if (f.output) f.output->Release();
    if (f.motionUav) f.motionUav->Release();
    if (f.motion) f.motion->Release();
    if (f.depthUav) f.depthUav->Release();
    if (f.depth) f.depth->Release();
    f = Feature{};
}

void ReleaseFamily(Family& f)
{
    if (f.depthSrv) f.depthSrv->Release();
    if (f.velocitySrv) f.velocitySrv->Release();
    if (f.depth) f.depth->Release();
    if (f.velocity) f.velocity->Release();
    f = Family{};
}

// Takes ownership of one reference on each resource.
void LearnFamily(std::uintptr_t depthKey, ID3D11Resource* depth, ID3D11Resource* velocity)
{
    AcquireSRWLockExclusive(&g_familyLock);
    Family* slot = nullptr;
    for (auto& f : g_family)
        if (f.depthKey == depthKey)
            slot = &f;
    if (slot && slot->velocity == velocity)
    {
        slot->lastSeen = g_present;
        ReleaseSRWLockExclusive(&g_familyLock);
        depth->Release();
        velocity->Release();
        return;
    }
    if (!slot)
    {
        for (auto& f : g_family)
            if (!f.depthKey) { slot = &f; break; }
        if (!slot)
        {
            slot = &g_family[0];
            for (auto& f : g_family)
                if (f.lastSeen < slot->lastSeen)
                    slot = &f;
        }
    }
    ReleaseFamily(*slot);
    slot->depthKey = depthKey;
    slot->depth = depth;
    slot->velocity = velocity;
    slot->lastSeen = g_present;
    ReleaseSRWLockExclusive(&g_familyLock);
}

DXGI_FORMAT DepthReadFormat(DXGI_FORMAT typeless)
{
    switch (typeless)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool CreateTexture(UINT width, UINT height, DXGI_FORMAT format, ID3D11Texture2D** texture,
    ID3D11UnorderedAccessView** uav)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, texture)) || !*texture)
        return false;
    if (uav && (FAILED(g_device->CreateUnorderedAccessView(*texture, nullptr, uav)) || !*uav))
        return false;
    return true;
}

// [DLSSMEM] Video memory of the game's adapter, in MB.  Over budget means the
// driver is paging textures to system memory: the frame rate collapses to 1-2.
bool VideoMemory(unsigned* usedMb, unsigned* budgetMb)
{
    if (!g_device)
        return false;
    bool ok = false;
    IDXGIDevice* dxgi{};
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice),
            reinterpret_cast<void**>(&dxgi))) && dxgi)
    {
        IDXGIAdapter* adapter{};
        if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter)
        {
            IDXGIAdapter3* adapter3{};
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                    reinterpret_cast<void**>(&adapter3))) && adapter3)
            {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,
                        DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) && info.Budget)
                {
                    *usedMb = static_cast<unsigned>(info.CurrentUsage >> 20);
                    *budgetMb = static_cast<unsigned>(info.Budget >> 20);
                    ok = true;
                }
                adapter3->Release();
            }
            adapter->Release();
        }
        dxgi->Release();
    }
    return ok;
}

void ReleaseFeature(Feature& f);

Feature* FeatureFor(ID3D11DeviceContext* context, std::uintptr_t depthKey, UINT width,
    UINT height, DXGI_FORMAT format)
{
    if (width == g_refusedWidth && height == g_refusedHeight)
        return nullptr;
    // [DLSSMEM] A family that is no longer drawn (the other VR mode's) gives
    // its memory back before a new one is made.
    bool known = false;
    for (auto& f : g_feature)
        known = known || (f.depthKey == depthKey && f.width == width && f.height == height &&
            f.format == format);
    if (!known)
        for (auto& f : g_feature)
            if (f.depthKey && f.depthKey != depthKey && g_present - f.lastPresent > 30)
            {
                Logf("[DLSSMEM] released the idle feature of eye family %p (%ux%u)",
                    reinterpret_cast<void*>(f.depthKey), f.width, f.height);
                ReleaseFeature(f);
            }
    unsigned usedBefore = 0, budget = 0;
    const bool haveMemory = !known && VideoMemory(&usedBefore, &budget);
    struct MemoryNote
    {
        bool on; unsigned before; UINT w, h;
        ~MemoryNote()
        {
            unsigned used = 0, budget = 0;
            if (on && VideoMemory(&used, &budget))
                Logf("[DLSSMEM] feature for %ux%u: video memory %u -> %u MB of %u MB budget",
                    w, h, before, used, budget);
        }
    } memoryNote{haveMemory, usedBefore, width, height};
    Feature* freeSlot = nullptr;
    for (auto& f : g_feature)
    {
        if (f.depthKey == depthKey)
        {
            if (f.width == width && f.height == height && f.format == format)
                return &f;
            Logf("[DLSS] eye family %p resized to %ux%u - recreating its feature",
                reinterpret_cast<void*>(depthKey), width, height);
            ReleaseFeature(f);
            freeSlot = &f;
            break;
        }
        if (!f.depthKey && !freeSlot)
            freeSlot = &f;
    }
    if (!freeSlot)
    {
        // More families than slots (stereo toggles recreate them): recycle the
        // one used least recently.
        freeSlot = &g_feature[0];
        for (auto& f : g_feature)
            if (f.lastPresent < freeSlot->lastPresent)
                freeSlot = &f;
        ReleaseFeature(*freeSlot);
    }

    Feature& f = *freeSlot;
    f.depthKey = depthKey;
    f.width = width;
    f.height = height;
    f.format = format;
    if (!CreateTexture(width, height, format, &f.output, nullptr) ||
        !CreateTexture(width, height, DXGI_FORMAT_R16G16_FLOAT, &f.motion, &f.motionUav) ||
        !CreateTexture(width, height, DXGI_FORMAT_R32_FLOAT, &f.depth, &f.depthUav))
    {
        ReleaseFeature(f);
        g_refusedWidth = width;
        g_refusedHeight = height;
        Logf("[DLSSSIZE] no memory for DLSS working textures at %ux%u. The engine's TAA runs "
             "at this render size; DLSS returns when the size changes.", width, height);
        return nullptr;
    }

    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        NVSDK_NGX_DLSS_Hint_Render_Preset_K);
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InHeight = height;
    create.Feature.InTargetHeight = height;
    create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    const int firstCount = (g_tileHintWidth == width && g_tileHintHeight == height)
        ? g_tileHint : 1;
    bool created = false;
    for (int count = firstCount; count <= kMaxTiles && !created; ++count)
    {
        const UINT tileWidth = count == 1 ? width
            : ((width + count - 1) / count + 2 * kTileOverlap < width ? (width + count - 1) / count + 2 * kTileOverlap : width);
        create.Feature.InWidth = tileWidth;
        create.Feature.InTargetWidth = tileWidth;
        bool all = true;
        for (int i = 0; i < count && all; ++i)
        {
            NVSDK_NGX_Handle** handle = count == 1 ? &f.handle : &f.tileHandle[i];
            result = NGX_D3D11_CREATE_DLSS_EXT(context, handle, g_params, &create);
            all = !NVSDK_NGX_FAILED(result) && *handle;
        }
        if (all && count > 1)
            all = CreateTexture(tileWidth, height, format, &f.tileOutput, nullptr) &&
                CreateTexture(tileWidth, height, DXGI_FORMAT_R16G16_FLOAT, &f.tileMotion, nullptr) &&
                CreateTexture(tileWidth, height, DXGI_FORMAT_R32_FLOAT, &f.tileDepth, nullptr);
        if (all)
        {
            created = true;
            f.tiles = count;
            f.tileWidth = tileWidth;
            if (g_tileHintWidth != width || g_tileHintHeight != height || g_tileHint != count)
                Logf("[DLSSTILE] %ux%u resolves as %d column(s) of %ux%u", width, height, count,
                    tileWidth, height);
            g_tileHintWidth = width;
            g_tileHintHeight = height;
            g_tileHint = count;
            break;
        }
        Logf("[DLSSTILE] NGX refused %ux%u as %d column(s) of %ux%u (code 0x%llX)", width,
            height, count, tileWidth, height,
            static_cast<unsigned long long>(static_cast<unsigned>(result)));
        for (auto*& h : f.tileHandle)
        {
            if (h) NVSDK_NGX_D3D11_ReleaseFeature(h);
            h = nullptr;
        }
        if (f.handle) NVSDK_NGX_D3D11_ReleaseFeature(f.handle);
        f.handle = nullptr;
        if (f.tileOutput) { f.tileOutput->Release(); f.tileOutput = nullptr; }
        if (f.tileMotion) { f.tileMotion->Release(); f.tileMotion = nullptr; }
        if (f.tileDepth) { f.tileDepth->Release(); f.tileDepth = nullptr; }
    }
    if (!created)
    {
        ReleaseFeature(f);
        g_refusedWidth = width;
        g_refusedHeight = height;
        Logf("[DLSSSIZE] DLSS refused %ux%u (code 0x%llX). The engine's TAA runs at this "
             "render size; DLSS returns when the size changes.", width, height,
            static_cast<unsigned long long>(static_cast<unsigned>(result)));
        return nullptr;
    }
    Logf("[DLSS] feature created for eye family %p: %ux%u format %d (DLAA, preset K)",
        reinterpret_cast<void*>(depthKey), width, height, static_cast<int>(format));
    return &f;
}

// Compute-stage state, saved around this module's pass and NGX's.  The engine rebinds
// what it needs per draw, but compute bindings left behind by this module must not leak
// into its next dispatch.
struct CsState
{
    ID3D11ComputeShader* shader{};
    ID3D11Buffer* cbs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
    ID3D11ShaderResourceView* srvs[16]{};
    ID3D11UnorderedAccessView* uavs[8]{};
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};

    void Save(ID3D11DeviceContext* c)
    {
        c->CSGetShader(&shader, nullptr, nullptr);
        c->CSGetConstantBuffers(0, ARRAYSIZE(cbs), cbs);
        c->CSGetShaderResources(0, ARRAYSIZE(srvs), srvs);
        c->CSGetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs);
        c->CSGetSamplers(0, ARRAYSIZE(samplers), samplers);
    }
    void Restore(ID3D11DeviceContext* c)
    {
        c->CSSetShader(shader, nullptr, 0);
        c->CSSetConstantBuffers(0, ARRAYSIZE(cbs), cbs);
        c->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
        c->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
        c->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);
        if (shader) shader->Release();
        for (auto* p : cbs) if (p) p->Release();
        for (auto* p : srvs) if (p) p->Release();
        for (auto* p : uavs) if (p) p->Release();
        for (auto* p : samplers) if (p) p->Release();
    }
};

bool IsParameterBufferUncached(ID3D11Resource* resource)
{
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
        return false;
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    return desc.ByteWidth == 256 && (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

// [TYPECACHE] This ran GetType+GetDesc on every Map (~3800 a
// frame on the render thread).  Per-resource answer cache, flushed every 600
// presents from OnPresent so a recycled pointer cannot keep a stale answer.
struct ParamCacheEntry { std::atomic<std::uintptr_t> key{}; std::atomic<int> param{}; };
ParamCacheEntry g_paramCache[1024];
void FlushParamCache() { for (auto& e : g_paramCache) e.key.store(0, std::memory_order_relaxed); }
bool IsParameterBuffer(ID3D11Resource* resource)
{
    const auto key = reinterpret_cast<std::uintptr_t>(resource);
    auto& e = g_paramCache[((key >> 4) ^ (key >> 14)) & 1023];
    if (e.key.load(std::memory_order_acquire) == key)
        return e.param.load(std::memory_order_relaxed) != 0;
    const bool param = IsParameterBufferUncached(resource);
    e.param.store(param ? 1 : 0, std::memory_order_relaxed);
    e.key.store(key, std::memory_order_release);
    return param;
}

bool LooksLikeTemporalParams(const float* d)
{
    return d[0] >= 256.0f && d[0] <= 16384.0f && d[1] >= 256.0f && d[1] <= 16384.0f &&
        std::fabs(d[2] * d[0] - 1.0f) < 1e-3f && std::fabs(d[3] * d[1] - 1.0f) < 1e-3f;
}

void Record(std::uintptr_t resource, const float* head)
{
    if (!LooksLikeTemporalParams(head))
        return;
    AcquireSRWLockExclusive(&g_captureLock);
    Capture& c = g_capture[g_captureSeq % kCaptures];
    c.resource = resource;
    c.seq = ++g_captureSeq;
    c.consumed = false;
    std::memcpy(c.data, head, sizeof(c.data));
    ++g_captures;
    ReleaseSRWLockExclusive(&g_captureLock);
}

// Newest unconsumed capture written to `resource`; marks it consumed.
bool TakeCapture(std::uintptr_t resource, float* out)
{
    AcquireSRWLockExclusive(&g_captureLock);
    Capture* best = nullptr;
    for (auto& c : g_capture)
        if (c.resource == resource && !c.consumed && (!best || c.seq > best->seq))
            best = &c;
    if (best)
    {
        best->consumed = true;
        std::memcpy(out, best->data, sizeof(best->data));
    }
    ReleaseSRWLockExclusive(&g_captureLock);
    return best != nullptr;
}

UINT TextureSize(ID3D11Resource* resource, UINT* height, DXGI_FORMAT* format, UINT* samples)
{
    ID3D11Texture2D* texture{};
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture))) || !texture)
        return 0;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    texture->Release();
    if (height) *height = desc.Height;
    if (format) *format = desc.Format;
    if (samples) *samples = desc.SampleDesc.Count;
    return desc.Width;
}

// Motion-vector prep + DLSS for one eye.  `cb` holds cbTemporalAA (the
// engine's own buffer for the resolved eye, a copy for the second eye).
bool Evaluate(ID3D11DeviceContext* context, Feature* feature, ID3D11Resource* colour,
    ID3D11ShaderResourceView* velocityView, ID3D11ShaderResourceView* depthView,
    ID3D11Buffer* cb, float jitterU, float jitterV, UINT width, UINT height, float* jitterOut)
{
    bool reset = feature->evaluations == 0 || g_present - feature->lastPresent > 2;
    CsState saved;
    saved.Save(context);

    ID3D11ShaderResourceView* inputs[2] = {velocityView, depthView};
    ID3D11UnorderedAccessView* outputs[2] = {feature->motionUav, feature->depthUav};
    context->CSSetShader(g_prepare, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &cb);
    {
        // [MVFIX] this family's real camera motion for the prepare shader.
        if (!g_motionCb)
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 208;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (g_device) g_device->CreateBuffer(&bd, nullptr, &g_motionCb);
        }
        if (g_motionCb)
        {
            float data[52];
            g_cutRequested = false;
            BuildMotionFix(feature->depthKey, data, static_cast<float>(width), static_cast<float>(height));
            if (g_cutRequested && kCutReset)
            {
                reset = true;   // [CUTRESET] fresh history instead of a smeared frame
                g_cutRequested = false;
                static std::uint64_t s_cutLogs = 0;
                if (++s_cutLogs <= 20 || (s_cutLogs % 200) == 0)
                    Logf("[DLSSMV] cut: DLSS history reset for %s (lens/turn/jump so far %llu/%llu/%llu)",
                        FamilyTag(feature->depthKey), static_cast<unsigned long long>(g_cutsLens),
                        static_cast<unsigned long long>(g_cutsTurn), static_cast<unsigned long long>(g_cutsJump));
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(g_motionCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                std::memcpy(mapped.pData, data, sizeof(data));
                context->Unmap(g_motionCb, 0);
            }
            context->CSSetConstantBuffers(1, 1, &g_motionCb);
        }
    }
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrv[2]{};
    ID3D11UnorderedAccessView* noUav[2]{};
    context->CSSetShaderResources(0, 2, noSrv);
    context->CSSetUnorderedAccessViews(0, 2, noUav, nullptr);

    NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = colour;
    eval.Feature.pInOutput = feature->output;
    eval.pInDepth = feature->depth;
    eval.pInMotionVectors = feature->motion;
    eval.InJitterOffsetX = jitterU * static_cast<float>(width);
    eval.InJitterOffsetY = jitterV * static_cast<float>(height);
    eval.InRenderSubrectDimensions.Width = width;
    eval.InRenderSubrectDimensions.Height = height;
    eval.InReset = reset ? 1 : 0;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
    if (feature->tiles <= 1)
        result = NGX_D3D11_EVALUATE_DLSS_EXT(context, feature->handle, g_params, &eval);
    else
    {
        // [DLSSTILE] NGX only ever sees column-sized textures.
        const UINT tileWidth = feature->tileWidth;
        if (!feature->tileColour)
        {
            ID3D11Texture2D* colourTexture{};
            if (SUCCEEDED(colour->QueryInterface(__uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&colourTexture))) && colourTexture)
            {
                D3D11_TEXTURE2D_DESC cd{};
                colourTexture->GetDesc(&cd);
                colourTexture->Release();
                cd.Width = tileWidth;
                cd.Height = height;
                cd.MipLevels = 1;
                cd.ArraySize = 1;
                cd.SampleDesc.Count = 1;
                cd.SampleDesc.Quality = 0;
                cd.Usage = D3D11_USAGE_DEFAULT;
                cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                cd.CPUAccessFlags = 0;
                cd.MiscFlags = 0;
                g_device->CreateTexture2D(&cd, nullptr, &feature->tileColour);
            }
        }
        if (!feature->tileColour)
            result = NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
        for (int i = 0; i < feature->tiles && !NVSDK_NGX_FAILED(result); ++i)
        {
            const UINT core0 = static_cast<UINT>((static_cast<std::uint64_t>(width) * i) /
                feature->tiles);
            const UINT core1 = static_cast<UINT>((static_cast<std::uint64_t>(width) * (i + 1)) /
                feature->tiles);
            UINT x0 = core0 > kTileOverlap ? core0 - kTileOverlap : 0;
            if (x0 + tileWidth > width)
                x0 = width - tileWidth;
            const D3D11_BOX in{x0, 0, 0, x0 + tileWidth, height, 1};
            context->CopySubresourceRegion(feature->tileColour, 0, 0, 0, 0, colour, 0, &in);
            context->CopySubresourceRegion(feature->tileMotion, 0, 0, 0, 0, feature->motion, 0, &in);
            context->CopySubresourceRegion(feature->tileDepth, 0, 0, 0, 0, feature->depth, 0, &in);
            eval.Feature.pInColor = feature->tileColour;
            eval.Feature.pInOutput = feature->tileOutput;
            eval.pInDepth = feature->tileDepth;
            eval.pInMotionVectors = feature->tileMotion;
            eval.InRenderSubrectDimensions.Width = tileWidth;
            eval.InRenderSubrectDimensions.Height = height;
            result = NGX_D3D11_EVALUATE_DLSS_EXT(context, feature->tileHandle[i], g_params, &eval);
            if (NVSDK_NGX_FAILED(result))
                break;
            const D3D11_BOX out{core0 - x0, 0, 0, core1 - x0, height, 1};
            context->CopySubresourceRegion(feature->output, 0, core0, 0, 0, feature->tileOutput,
                0, &out);
        }
    }

    saved.Restore(context);
    if (NVSDK_NGX_FAILED(result))
    {
        g_refusedWidth = width;
        g_refusedHeight = height;
        Logf("[DLSSSIZE] DLSS evaluate failed at %ux%u in %d column(s) (code 0x%llX). The "
             "engine's TAA runs at this render size.", width, height, feature->tiles,
            static_cast<unsigned long long>(static_cast<unsigned>(result)));
        return false;
    }
    feature->lastPresent = g_present;
    ++feature->evaluations;
    if (jitterOut)
    {
        jitterOut[0] = eval.InJitterOffsetX;
        jitterOut[1] = eval.InJitterOffsetY;
    }
    return true;
}

// The second eye: DLSS in place on the shared scene colour, just before the
// draw that follows the resolve in the first eye.  Never skips the draw.
void RunSecondEye(ID3D11DeviceContext* context)
{
    // Scene colour as this draw reads it.
    ID3D11ShaderResourceView* srv[16]{};
    context->PSGetShaderResources(0, 16, srv);
    ID3D11Resource* colour{};
    int colourSlot = -1;
    for (int i = 0; i < 16 && !colour; ++i)
    {
        if (!srv[i])
            continue;
        ID3D11Resource* r{};
        srv[i]->GetResource(&r);
        if (r && reinterpret_cast<std::uintptr_t>(r) == g_sceneColour)
        {
            colour = r;
            colourSlot = i;
        }
        else if (r)
            r->Release();
    }
    for (auto* v : srv)
        if (v) v->Release();

    do
    {
        if (!colour)
        {
            ++g_secondNoColour;
            if (!g_loggedNoColour)
            {
                g_loggedNoColour = true;
                Logf("[DLSS] second eye: the post draw after the resolve does not read the scene "
                     "colour in this eye - second-eye DLSS cannot attach here (see DLSSTRACE).");
            }
            break;
        }
        if (!g_haveParams)
        {
            ++g_secondNoParams;
            break;
        }
        UINT height = 0, samples = 0;
        DXGI_FORMAT format{};
        const UINT width = TextureSize(colour, &height, &format, &samples);
        // This frame's jitter: the resolved eye has not rendered yet.
        float predictedPx[2]{};
        if (!PredictJitter(predictedPx))
        {
            ++g_secondNoPrediction;
            break;
        }
        g_predicted[0] = predictedPx[0];
        g_predicted[1] = predictedPx[1];
        g_predictedPresent = g_present;
        const float predictedU = predictedPx[0] / static_cast<float>(width ? width : 1);
        const float predictedV = predictedPx[1] / static_cast<float>(height ? height : 1);
        g_lastParams[8] = predictedU;
        g_lastParams[9] = predictedV;
        AcquireSRWLockShared(&g_familyLock);
        Family* family = nullptr;
        for (auto& f : g_family)
            if (f.depthKey == g_currentScene)
                family = &f;
        ID3D11Resource* depthRes = family ? family->depth : nullptr;
        ID3D11Resource* velocityRes = family ? family->velocity : nullptr;
        if (depthRes) depthRes->AddRef();
        if (velocityRes) velocityRes->AddRef();
        ReleaseSRWLockShared(&g_familyLock);
        if (!depthRes || !velocityRes)
        {
            if (depthRes) depthRes->Release();
            if (velocityRes) velocityRes->Release();
            ++g_secondNoFamily;
            break;
        }

        UINT depthHeight = 0, velocityHeight = 0;
        DXGI_FORMAT depthFormat{};
        const UINT depthWidth = TextureSize(depthRes, &depthHeight, &depthFormat, nullptr);
        const UINT velocityWidth = TextureSize(velocityRes, &velocityHeight, nullptr, nullptr);
        ID3D11ShaderResourceView* velocityView{};
        ID3D11ShaderResourceView* depthView{};
        bool ok = width && samples == 1 && depthWidth == width && depthHeight == height &&
            velocityWidth == width && velocityHeight == height;
        if (ok)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = DXGI_FORMAT_R16G16_FLOAT;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;
            D3D11_SHADER_RESOURCE_VIEW_DESC dd = vd;
            dd.Format = DepthReadFormat(depthFormat);
            ok = dd.Format != DXGI_FORMAT_UNKNOWN &&
                SUCCEEDED(g_device->CreateShaderResourceView(velocityRes, &vd, &velocityView)) &&
                SUCCEEDED(g_device->CreateShaderResourceView(depthRes, &dd, &depthView));
        }
        depthRes->Release();
        velocityRes->Release();
        if (!ok)
        {
            if (velocityView) velocityView->Release();
            if (depthView) depthView->Release();
            ++g_secondBadSize;
            break;
        }

        if (!g_ownCb)
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(g_lastParams);
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(g_device->CreateBuffer(&bd, nullptr, &g_ownCb)))
            {
                velocityView->Release();
                depthView->Release();
                Fail("could not create the second-eye parameter buffer", 0);
                break;
            }
        }
        context->UpdateSubresource(g_ownCb, 0, nullptr, g_lastParams, 0, 0);

        Feature* feature = FeatureFor(context, g_currentScene, width, height, format);
        float jitter[2]{};
        const bool done = feature && Evaluate(context, feature, colour, velocityView, depthView,
            g_ownCb, g_lastParams[8], g_lastParams[9], width, height, jitter);
        velocityView->Release();
        depthView->Release();
        if (!done)
            break;
        context->CopyResource(colour, feature->output);
        ++g_secondEvaluations;
        if (g_present < g_markerUntil)
        {
            if (g_colourRtvKey != reinterpret_cast<std::uintptr_t>(colour))
            {
                if (g_colourRtv) g_colourRtv->Release();
                g_colourRtv = nullptr;
                g_colourRtvKey = 0;
                if (SUCCEEDED(g_device->CreateRenderTargetView(colour, nullptr, &g_colourRtv)))
                    g_colourRtvKey = reinterpret_cast<std::uintptr_t>(colour);
            }
            ID3D11DeviceContext1* context1{};
            if (g_colourRtv && SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                    reinterpret_cast<void**>(&context1))) && context1)
            {
                const float green[4] = {0.0f, 8.0f, 0.0f, 1.0f};
                const D3D11_RECT square{static_cast<LONG>(width * 46 / 100),
                    static_cast<LONG>(height * 20 / 100), static_cast<LONG>(width * 54 / 100),
                    static_cast<LONG>(height * 20 / 100 + width * 8 / 100)};
                context1->ClearView(g_colourRtv, green, &square, 1);
                context1->Release();
            }
        }
        if (g_paramsPresent == g_present)
            ++g_secondSameFrameParams;
        Trace("SECOND-EYE DLSS on scene colour (slot %d of the anchor draw), family %p, "
              "predicted jitter (%.3f, %.3f) px, motion matrix from %s",
            colourSlot, reinterpret_cast<void*>(g_currentScene), jitter[0], jitter[1],
            g_paramsPresent == g_present ? "this frame" : "the previous frame");
        if (!g_loggedSecond)
        {
            g_loggedSecond = true;
            Logf("[DLSS] FIRST SECOND-EYE DLSS FRAME: %ux%u in place on the scene colour, family "
                 "%p, jitter (%.3f, %.3f) px.", width, height,
                reinterpret_cast<void*>(g_currentScene), jitter[0], jitter[1]);
        }
    } while (false);
    if (colour) colour->Release();
}

} // namespace

namespace DlssUpscaler
{

void SetAerBypass(bool enabled)
{
    g_aerBypass.store(enabled, std::memory_order_release);
}

bool OnDraw(ID3D11DeviceContext* context)
{
    if (!g_initDone)
        InitIni();
    if (g_failed || !context)
        return false;
    if (g_aerBypass.load(std::memory_order_acquire))
        return false;
    const std::uintptr_t resolve = DlssProbe::TemporalResolveShader();
    if (!resolve)
        return false;
    const bool immediate = reinterpret_cast<std::uintptr_t>(context) ==
        DlssProbe::ImmediateContextKey();

    ID3D11PixelShader* shader{};
    context->PSGetShader(&shader, nullptr, nullptr);
    const auto key = reinterpret_cast<std::uintptr_t>(shader);
    if (shader)
        shader->Release();

    if (key != resolve)
    {
        if (!immediate)
            return false;
        if (g_traceState == 2 && g_sceneColour)
        {
            ID3D11ShaderResourceView* srv[16]{};
            context->PSGetShaderResources(0, 16, srv);
            int slot = -1;
            for (int i = 0; i < 16; ++i)
            {
                if (!srv[i]) continue;
                ID3D11Resource* r{};
                srv[i]->GetResource(&r);
                if (r && slot < 0 && reinterpret_cast<std::uintptr_t>(r) == g_sceneColour)
                    slot = i;
                if (r) r->Release();
                srv[i]->Release();
            }
            if (slot >= 0)
                Trace("draw ps=%p reads scene colour at slot %d during %s scene%s",
                    reinterpret_cast<void*>(key), slot, FamilyTag(g_currentScene),
                    key == g_anchorPs ? " [ANCHOR]" : "");
        }
        if (g_glareFix && g_currentScene && g_resolvedDepth)   // [GLAREFIX] off: no per-draw SRV reads
        {
            if (g_currentScene == g_resolvedDepth)
                RecordBindings(context, key, false);
            else
                FillEmptyInputs(context, key, false);
        }
        if (g_learnAnchor && g_sceneColour && ReadsSceneColour(context))
        {
            // First scene-colour reader after the resolve: where the resolve sits
            // in the other eye's otherwise identical chain.
            g_learnAnchor = false;
            if (g_anchorPs != key)
            {
                g_anchorPs = key;
                if (!g_loggedAnchor)
                {
                    g_loggedAnchor = true;
                    Logf("[DLSS] learnt the anchor: ps=%p is the first draw after the resolve that "
                         "reads the scene colour. The second eye gets DLSS just before it.",
                        reinterpret_cast<void*>(key));
                }
            }
            Trace("anchor ps=%p (first scene-colour reader after the resolve)",
                reinterpret_cast<void*>(key));
        }
        if (!g_active || !g_ngxReady || key != g_anchorPs || g_secondDone)
            return false;
        if (!g_currentScene || g_currentScene == g_resolvedDepth)
            return false;
        g_secondDone = true;
        RunSecondEye(context);
        return false;
    }

    if (immediate)
    {
        g_lastResolvePresent = g_present;
        g_learnAnchor = true;
        // Learnt whether or not DLSS is on, so the flash fix also works over TAA.
        ID3D11ShaderResourceView* c{};
        ID3D11ShaderResourceView* d{};
        context->PSGetShaderResources(0, 1, &c);
        context->PSGetShaderResources(3, 1, &d);
        ID3D11RenderTargetView* o{};
        context->OMGetRenderTargets(1, &o, nullptr);
        ID3D11Resource* r{};
        if (c) { c->GetResource(&r); if (r) { g_sceneColour = reinterpret_cast<std::uintptr_t>(r); r->Release(); } c->Release(); }
        if (d) { d->GetResource(&r); if (r) { g_resolvedDepth = reinterpret_cast<std::uintptr_t>(r); r->Release(); } d->Release(); }
        if (o) { o->GetResource(&r); if (r) { g_resolveOutput = reinterpret_cast<std::uintptr_t>(r); r->Release(); } o->Release(); }
    }
    Trace("RESOLVE draw during %s scene%s", FamilyTag(g_currentScene),
        g_active ? " -> DLSS" : " (engine TAA)");
    if (!g_active)
        return false;
    if (!g_ngxReady && !InitNgx(context))
        return false;
    if (context != g_immediate)
    {
        ++g_skipDeferred;
        if (!g_loggedDeferred)
        {
            g_loggedDeferred = true;
            Logf("[DLSS] the resolve is recorded on a DEFERRED context; this build only acts on "
                 "the immediate context, so the engine's TAA keeps running there.");
        }
        return false;
    }

    // Inputs exactly as the resolve binds them.
    ID3D11ShaderResourceView* colourView{};
    ID3D11ShaderResourceView* depthView{};
    ID3D11ShaderResourceView* velocityView{};
    context->PSGetShaderResources(0, 1, &colourView);
    context->PSGetShaderResources(3, 1, &depthView);
    context->PSGetShaderResources(6, 1, &velocityView);
    ID3D11Buffer* cb{};
    context->PSGetConstantBuffers(0, 1, &cb);
    ID3D11RenderTargetView* rtv{};
    context->OMGetRenderTargets(1, &rtv, nullptr);

    bool handled = false;
    ID3D11Resource* colour{};
    ID3D11Resource* depth{};
    ID3D11Resource* target{};
    if (colourView) colourView->GetResource(&colour);
    if (depthView) depthView->GetResource(&depth);
    if (rtv) rtv->GetResource(&target);

    do
    {
        if (!colour || !depth || !velocityView || !cb || !target)
        {
            ++g_skipBadInputs;
            break;
        }
        UINT width = 0, height = 0, targetHeight = 0, samples = 0;
        DXGI_FORMAT format{};
        width = TextureSize(target, &height, &format, &samples);
        UINT colourHeight = 0;
        const UINT colourWidth = TextureSize(colour, &colourHeight, nullptr, nullptr);
        targetHeight = height;
        if (!width || samples != 1 || colourWidth != width || colourHeight != targetHeight)
        {
            ++g_skipBadInputs;
            break;
        }

        // Jitter: the last write to this exact parameter buffer before the draw
        // is what the engine's resolve would read.
        float params[kShadowFloats]{};
        if (!TakeCapture(reinterpret_cast<std::uintptr_t>(cb), params))
        {
            ++g_skipNoShadow;
            break;
        }
        const float jitterU = params[8];
        const float jitterV = params[9];
        std::memcpy(g_lastMotion, params + 12, sizeof(g_lastMotion));   // [MVFIX] self-check reference
        g_motionFresh = true;
        if (!g_loggedShadow)
        {
            g_loggedShadow = true;
            Logf("[DLSS] cbTemporalAA first capture: screenSize=(%.1f, %.1f, %.6f, %.6f) "
                 "frameBits=(%.3f, %.3f, %.3f, %.3f) uvJitter=(%.7f, %.7f, %.7f, %.7f); "
                 "target %ux%u",
                params[0], params[1], params[2], params[3], params[4], params[5], params[6],
                params[7], params[8], params[9], params[10], params[11], width, height);
        }

        Feature* feature = FeatureFor(context, reinterpret_cast<std::uintptr_t>(depth), width,
            height, format);
        if (!feature)
            break;
        float jitter[2]{};
        if (!Evaluate(context, feature, colour, velocityView, depthView, cb, jitterU, jitterV,
                width, height, jitter))
            break;
        LearnJitter(jitter[0], jitter[1]);
        g_sceneColour = reinterpret_cast<std::uintptr_t>(colour);
        g_resolvedDepth = reinterpret_cast<std::uintptr_t>(depth);
        std::memcpy(g_lastParams, params, sizeof(float) * kShadowFloats);
        g_haveParams = true;
        g_paramsPresent = g_present;

        // 3. Into the engine's resolve target; the engine's draw is skipped.
        context->CopyResource(target, feature->output);
        if (g_present < g_markerUntil)
        {
            ID3D11DeviceContext1* context1{};
            if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                    reinterpret_cast<void**>(&context1))) && context1)
            {
                const float green[4] = {0.0f, 8.0f, 0.0f, 1.0f};   // HDR target: bright
                const D3D11_RECT square{static_cast<LONG>(width * 46 / 100),
                    static_cast<LONG>(height * 20 / 100), static_cast<LONG>(width * 54 / 100),
                    static_cast<LONG>(height * 20 / 100 + width * 8 / 100)};
                context1->ClearView(rtv, green, &square, 1);
                context1->Release();
            }
        }
        ++g_evaluations;
        g_lastJitterX = jitter[0];
        g_lastJitterY = jitter[1];
        if (g_evaluations == 1)
            Logf("[DLSS] FIRST DLSS FRAME: %ux%u, jitter (%.3f, %.3f) px. The engine's TAA draw "
                 "is now replaced.", width, height, jitter[0], jitter[1]);
        handled = true;
    } while (false);

    if (colour) colour->Release();
    if (depth) depth->Release();
    if (target) target->Release();
    if (colourView) colourView->Release();
    if (depthView) depthView->Release();
    if (velocityView) velocityView->Release();
    if (cb) cb->Release();
    if (rtv) rtv->Release();
    return handled;
}

void RequestPairBurst(unsigned presents)
{
    g_burstUntil = g_present + presents;
}

void OnGeometryDraw(ID3D11DeviceContext* context)
{
    const std::uintptr_t family = t_pendingFamily;
    if (!family || !context)
        return;
    bool any = false;
    for (UINT slot = 0; slot < static_cast<UINT>(kCamSlots); ++slot)
    {
        ID3D11Buffer* cb{};
        context->VSGetConstantBuffers(slot, 1, &cb);
        if (!cb) continue;
        float f[192];
        const bool found = MvFix::Lookup(cb, f);
        cb->Release();
        if (!found) continue;
        StoreFamilyCam(family, f, static_cast<int>(slot));
        any = true;
    }
    if (any || --t_pendingBudget <= 0)
        t_pendingFamily = 0;
    if (any && g_camSlotLogged < 0)
    {
        g_camSlotLogged = 1;
        Logf("[DLSSMV] v6: eye cameras captured from the view constants (VS slots 0-3) in each velocity pass");
    }
}

void OnDispatch(ID3D11DeviceContext* context)
{
    // [KEEPALIVE] This compute-stage recorder stays ON.  It holds a
    // reference to every compute view the engine binds, which covers a
    // premature release seen in the engine's state apply on compute stage 5 /
    // slot 8 (an already released view).  Cheap (per dispatch, ~100 a frame);
    // the per-DRAW recorder stays off.
    if (g_failed || !context || !g_currentScene || !g_resolvedDepth)
        return;
    if (reinterpret_cast<std::uintptr_t>(context) != DlssProbe::ImmediateContextKey())
        return;
    ID3D11ComputeShader* shader{};
    context->CSGetShader(&shader, nullptr, nullptr);
    const auto key = reinterpret_cast<std::uintptr_t>(shader);
    if (shader)
        shader->Release();
    if (!key)
        return;
    if (g_currentScene == g_resolvedDepth)
        RecordBindings(context, key, true);
    else
        FillEmptyInputs(context, key, true);
}

void OnMapped(ID3D11Resource* resource, D3D11_MAP type, void* data)
{
    if (!g_active || g_failed || !resource || !data)
        return;
    if (type != D3D11_MAP_WRITE_DISCARD && type != D3D11_MAP_WRITE_NO_OVERWRITE &&
        type != D3D11_MAP_WRITE)
        return;
    if (!IsParameterBuffer(resource))
        return;
    // Map and Unmap of one write pair up on the recording thread.
    t_mappedResource = reinterpret_cast<std::uintptr_t>(resource);
    t_mappedData = data;
}

void OnUnmapping(ID3D11Resource* resource)
{
    if (!resource || reinterpret_cast<std::uintptr_t>(resource) != t_mappedResource ||
        !t_mappedData)
        return;
    // Out of write-combined memory, before the engine's Unmap: 16 bytes to test
    // the signature, the rest (c0..c6, 112 bytes) only for a match.
    const auto key = t_mappedResource;
    const void* data = t_mappedData;
    t_mappedResource = 0;
    t_mappedData = nullptr;
    float head[kShadowFloats];
    std::memcpy(head, data, sizeof(float) * 4);
    if (!LooksLikeTemporalParams(head))
        return;
    std::memcpy(head, data, sizeof(head));
    Record(key, head);
}

void OnUpdated(ID3D11Resource* resource, const D3D11_BOX* box, const void* data)
{
    if (!g_active || g_failed || !resource || !data || (box && box->left != 0))
        return;
    if (box && box->right < sizeof(float) * kShadowFloats)
        return;
    if (!IsParameterBuffer(resource))
        return;
    float head[kShadowFloats];
    std::memcpy(head, data, sizeof(head));
    Record(reinterpret_cast<std::uintptr_t>(resource), head);
}

void OnSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* dsv)
{
    if (!g_initDone)
        InitIni();
    if (g_failed || !context || !dsv || !views || count == 0 ||
        count > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dsv->GetDesc(&dd);
    if (dd.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT && dd.Format != DXGI_FORMAT_D32_FLOAT &&
        dd.Format != DXGI_FORMAT_D24_UNORM_S8_UINT)
        return;
    ID3D11Resource* depth{};
    dsv->GetResource(&depth);
    if (!depth)
        return;
    if (TextureSize(depth, nullptr, nullptr, nullptr) < 1024)
    {
        depth->Release();
        return;
    }
    const auto depthKey = reinterpret_cast<std::uintptr_t>(depth);

    // Family: a full-size RG16F target written together with this depth.
    for (UINT i = 0; i < count; ++i)
    {
        if (!views[i])
            continue;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        views[i]->GetDesc(&rd);
        if (rd.Format != DXGI_FORMAT_R16G16_FLOAT)
            continue;
        ID3D11Resource* velocity{};
        views[i]->GetResource(&velocity);
        if (!velocity)
            continue;
        if (TextureSize(velocity, nullptr, nullptr, nullptr) < 1024)
        {
            velocity->Release();
            continue;
        }
        depth->AddRef();
        LearnFamily(depthKey, depth, velocity);   // takes both references
        t_pendingFamily = depthKey;              // [MVFIX] capture this eye's cameras in the next draws
        t_pendingBudget = 64;
        break;
    }

    // Which eye's scene the immediate context is drawing: scene colour + depth.
    if (g_sceneColour && views[0] &&
        reinterpret_cast<std::uintptr_t>(context) == DlssProbe::ImmediateContextKey())
    {
        ID3D11Resource* rt{};
        views[0]->GetResource(&rt);
        if (rt && reinterpret_cast<std::uintptr_t>(rt) == g_sceneColour && g_currentScene != depthKey)
        {
            g_currentScene = depthKey;
            g_secondDone = false;
            ++g_sceneSwitchesThisPresent;
            Trace("scene colour now drawn with %s depth %p", FamilyTag(depthKey),
                reinterpret_cast<void*>(depthKey));
        }
        if (rt) rt->Release();
    }
    depth->Release();
}

void OnPresent()
{
    if (!g_initDone)
        InitIni();
    ++g_present;
    // [DLSSMEM] Over budget alone is NOT a failure: 50 % render size in
    // stereo sits ~3 GB over the reported budget and runs a steady 44 fps.
    // Paging shows as frames of hundreds of ms (the 100 % tiled run fell to
    // 280-670 ms per frame).  Only over budget AND that collapse, for three
    // checks in a row, releases DLSS.
    if ((g_present % 30) == 0)
    {
        static int overBudget = 0;
        static LARGE_INTEGER lastCheck{};
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const double frameMs = lastCheck.QuadPart && frequency.QuadPart
            ? double(now.QuadPart - lastCheck.QuadPart) * 1000.0 / double(frequency.QuadPart) / 30.0
            : 0.0;
        lastCheck = now;
        unsigned used = 0, budget = 0;
        bool any = false;
        UINT w = 0, h = 0;
        for (auto& f : g_feature)
            if (f.depthKey) { any = true; w = f.width; h = f.height; }
        if (any && frameMs > 150.0 && VideoMemory(&used, &budget) && used > budget)
        {
            Logf("[DLSSMEM] paging suspected: %.0f ms per frame, video memory %u of %u MB (%d/3)",
                frameMs, used, budget, overBudget + 1);
            if (++overBudget >= 3)
            {
                overBudget = 0;
                for (auto& f : g_feature)
                    if (f.depthKey) ReleaseFeature(f);
                g_refusedWidth = w;
                g_refusedHeight = h;
                Logf("[DLSSMEM] video memory over budget (%u of %u MB): DLSS released at "
                     "%ux%u, the engine's TAA runs. Lower the render size to get DLSS back.",
                    used, budget, w, h);
            }
        }
        else
            overBudget = 0;
    }
    static bool aerBypassLogged = false;
    const bool aerBypass = g_aerBypass.load(std::memory_order_acquire);
    if (aerBypass != aerBypassLogged)
    {
        aerBypassLogged = aerBypass;
        Logf("[DLSS] %s: engine TAA owns the alternating AER output",
            aerBypass ? "AER BYPASS" : "AER bypass cleared; integrated DLSS restored");
    }
    if (g_traceState == 2)
    {
        g_traceState = 3;
        Logf("[DLSSTRACE] end of traced frame (%d lines)", g_traceLines);
    }
    else if (g_traceState == 1)
    {
        g_traceState = 2;
        Logf("[DLSSTRACE] tracing one stereo frame: scene switches, reads of the scene colour, "
             "the resolve, the learnt successor and the second-eye pass, in order.");
    }
    else if (g_traceState == 0 && g_sceneSwitchesThisPresent >= 2 && g_anchorPs &&
             g_evaluations > 300)
        g_traceState = 1;
    g_sceneSwitchesThisPresent = 0;
    g_secondDone = false;
    if (g_resolvedDepth && g_present - g_lastResolvePresent > 30)
    {
        g_resolvedDepth = 0;
        g_sceneColour = 0;
        g_currentScene = 0;
        g_anchorPs = 0;
        g_learnAnchor = false;
        g_haveParams = false;
        if (!g_staleReset)
            Logf("[DLSS] the engine stopped running its TAA pass (AA not TAA?) - all TAA-mode "
                 "state dropped; nothing acts until it runs again.");
        g_staleReset = true;
    }
    else if (g_present - g_lastResolvePresent <= 30)
        g_staleReset = false;
    (void)g_numpad6Down; // [GLAREFIX] no hotkey
    if ((g_present % 600) == 0)
        FlushParamCache();
    if (g_present % kReportEvery == 0 && (g_evaluations || g_skipNoShadow || g_skipBadInputs ||
                                          g_skipDeferred || g_failed || g_glareFills))
        Logf("[DLSS] present=%u %s resolvedEye=%llu secondEye=%llu (jitterPrediction hit=%llu "
             "miss=%llu noPrediction=%llu; noColour=%llu noFamily=%llu noParams=%llu badSize=%llu) "
             "glareFix=%s filledSlots=%llu filledDraws=%llu paramCaptures=%llu "
             "skipped: noParams=%llu badInputs=%llu deferred=%llu last jitter=(%.3f, %.3f) px",
            g_present, g_failed ? "FAILED" : (g_active ? "DLSS" : "TAA"),
            static_cast<unsigned long long>(g_evaluations),
            static_cast<unsigned long long>(g_secondEvaluations),
            static_cast<unsigned long long>(g_predictHits),
            static_cast<unsigned long long>(g_predictMisses),
            static_cast<unsigned long long>(g_secondNoPrediction),
            static_cast<unsigned long long>(g_secondNoColour),
            static_cast<unsigned long long>(g_secondNoFamily),
            static_cast<unsigned long long>(g_secondNoParams),
            static_cast<unsigned long long>(g_secondBadSize),
            g_glareFix ? "on" : "off", static_cast<unsigned long long>(g_glareFills),
            static_cast<unsigned long long>(g_glareDraws),
            static_cast<unsigned long long>(g_captures),
            static_cast<unsigned long long>(g_skipNoShadow),
            static_cast<unsigned long long>(g_skipBadInputs),
            static_cast<unsigned long long>(g_skipDeferred), g_lastJitterX, g_lastJitterY);
}

} // namespace DlssUpscaler
