#include "transfer_batch.h"
#include "present_timing_capture.h"
#include "gameplay_log_filter.h"
#include "full_eye_capture.h"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include "openxr_presenter.h"
#include "head_pose_math.h"
#include "head_pose_timing.h"
#include "head_pose_association.h"
#include "world_lock_probe.h"
#include "runtime_log.h"
#include "gpu_pose_trace.h"
#include "native_image_hold.h"
#include "aer_projection_history.h"
#include "single_pose_continuity.h"
#include "native_draw_pose.h"
#include "aer_control.h"
#include "afw_synth.h"
#include "gbuffer_census.h"
#include "objective_pixel_probe.h"
#include "runtime_compatibility.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <strsafe.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kRenderedVerticalFovDegrees = 48.0f;
constexpr float kRenderedAspect = 16.0f / 9.0f;
constexpr float kLaneBFovScale = 1.300f;
constexpr std::uint64_t kRetryDelayMs = 5000;

// Live game projection published from the camera-source hook.  Zero focals
// mean "not captured yet" and fall back to the legacy runtime-FOV math.
// Fill factor: the game renders ~77x48deg into a ~100deg headset.  The
// game's own FOV fields are not writable (both candidates log-proven inert),
// so coverage is bought at the claim: widening the claimed frustum makes the
// compositor scale the image up to fill more of the view.  1.0 = geometry-
// honest with a black border; higher = fuller with mild magnification.
// Adjustable at runtime (see RetailXr::SetFillFactor / F7).
std::atomic<float> g_fillFactor{1.0f};
// Seeded with the measured gameplay lens (76.72 x 48.00 degrees) rather than
// zero.  A zero here would make the presenter fall back to claiming the
// HEADSET's frustum until the first lens measurement arrived, so the image
// would start large and shrink the moment the feed engaged.  Seeding it means
// the claim is honest from the first frame and never changes shape on its own.
std::atomic<float> g_gameFocalX{1.2634f};
std::atomic<float> g_gameFocalY{2.2460f};
std::atomic_bool g_afwEnabled{false};
std::atomic_bool g_afwFlip{false};
std::atomic_uint64_t g_aerRawTagFallbacks{};
std::atomic_uint64_t g_afwSynthesized{}, g_afwSkipped{};
std::atomic<float> g_gameSkew{0.0f};
std::atomic<float> g_gameReferenceFocalX{1.2634f};
std::atomic<float> g_gameReferenceFocalY{2.2460f};
std::atomic<float> g_lensSmoothedFocalY{0.0f};
std::atomic_int g_aerRenderEye{0};
std::atomic<float> g_aerConvergence{-0.0043f};   // [DEFAULTS] tested tuning

// [AERSEQ]
// The eye label is decided at CAMERA BUILD time on the game thread, but those
// pixels only reach a Present one or more frames later.  Reading a
// presenter-owned "next eye" arm at build time is therefore a cross-thread
// feedback loop: whenever the game thread runs ahead, or the pipeline depth
// changes because the area got heavy, the label and the pixels disagree and
// the world visibly vibrates between the two eye positions.  At a steady
// cadence the mislabel is constant and reads as a fixed eye swap; when the
// depth flaps it reads as flicker.
//
// The fix: the build derives its eye from its OWN
// sequence parity and queues {seq, eye}; the present consumes OLDEST-FIRST,
// one per present, matching swapchain delivery order.  The label can no
// longer disagree with the pixels.
constexpr std::uint64_t kAerRingCount = 8;

struct AerStampSlot
{
    std::atomic_uint64_t seq{0};
    std::atomic_int eye{0};
    // [AERTAG] which head sample this build was rendered with, and the camera
    // it committed.  Written before seq is released, re-checked after reading.
    std::atomic_uint64_t headSerial{0};
    float view[16]{};
    std::atomic_bool viewValid{false};
};

AerStampSlot g_aerRing[kAerRingCount];
std::atomic_uint64_t g_aerStampSeq{0};
std::atomic_bool g_aerStampArmed{};

// [MONO2D] Published once per present from the game side.
std::atomic_bool g_sceneIsFlat{};
std::atomic<unsigned long long> g_xrReadyTick{0};   // [VRHINT]
std::atomic<float> g_menuOffsetX{0.0f}, g_menuOffsetY{0.0f};   // [MENUDRAG]
std::atomic_bool g_dockFlatScreens{false};                        // [DOCK]
std::atomic_bool g_monoSubmit{false};                             // [MONOVR]
std::atomic_bool g_flatSceneMono{true};

// Consume the build after lastSeq, oldest first.  False means nothing new has
// been rendered since the last present.  If the wanted slot has already been
// overwritten - the consumer fell a whole ring behind on a load screen or a
// mode switch - resync to the newest build and say so.
bool ConsumeAerStamp(std::uint64_t lastSeq, std::uint64_t* outSeq, int* outEye,
    bool* resynced, std::uint64_t* outHeadSerial, float* outView, bool* outViewValid)
{
    *resynced = false;
    const auto newest = g_aerStampSeq.load(std::memory_order_acquire);
    if (newest <= lastSeq)
        return false;
    auto want = lastSeq + 1;
    const AerStampSlot* slot = &g_aerRing[want % kAerRingCount];
    if (slot->seq.load(std::memory_order_acquire) != want)
    {
        want = newest;
        *resynced = true;
        slot = &g_aerRing[want % kAerRingCount];
        if (slot->seq.load(std::memory_order_acquire) != want)
            return false;   // slot mid-write; take it next present
    }
    *outSeq = want;
    *outEye = slot->eye.load(std::memory_order_relaxed) & 1;
    *outHeadSerial = slot->headSerial.load(std::memory_order_relaxed);
    *outViewValid = slot->viewValid.load(std::memory_order_relaxed);
    std::memcpy(outView, slot->view, sizeof(slot->view));
    if (slot->seq.load(std::memory_order_acquire) != want)
    {
        // Overwritten while being read: keep the eye label, drop the pose.
        *outHeadSerial = 0;
        *outViewValid = false;
    }
    return true;
}

struct HeadPoseState
{
    SRWLOCK lock = SRWLOCK_INIT;
    XrPosef poses[2]{};
    XrVector3f centerPosition{};
    float centerYaw{};
    float centerPitch{};
    float centerRoll{};
    float yaw{};
    float pitch{};
    float roll{};
    float right{};
    float up{};
    float forward{};
    std::uint64_t serial{};
    bool centerValid{};
    bool valid{};
};

struct RenderedHeadPose
{
    XrPosef poses[2]{};
    std::uint64_t serial{};
    float markerView[16]{};
    int renderedEye{-1};
};

HeadPoseState g_headPose{};
// Protected by g_headPose.lock. No smoothing: these are the original samples.
HeadPoseAssociation::History<RenderedHeadPose> g_headCameraHistory;
HeadPoseAssociation::OrderedFrames<RenderedHeadPose> g_headFrameQueue;
std::atomic_uint64_t g_headQueueDrops{};
thread_local std::uint64_t g_headFrameQueueSequence{}, g_headFrameQueueAge{}, g_headFrameQueueDepth{};
std::uint64_t g_headPoseEpoch{1};
thread_local HeadPoseAssociation::FrameLatch<RenderedHeadPose> g_headFrameLatch;
std::atomic_uint64_t g_headCameraCommitMiss{}, g_headCameraMatch{}, g_headCameraMiss{};
std::atomic_uint64_t g_monoTagExact{}, g_monoTagBlend{}, g_monoTagNearest{}, g_monoTagMiss{};   // [MONOTAG]
std::atomic_uint32_t g_headCameraUploadThread{};
std::atomic_uint64_t g_headPoseExact{}, g_headPoseFallback{};
std::atomic<float> g_nativeHeadTiming{0.0f};
// Current versus queued sample remains an experimental native submission choice.
// GPU capture: rendered samples usually lag current by two serials;
// the FIFO is also often one serial older than the sampled GPU view. Neither
// route is proven to tag every final image exactly. Both must use the upright
// yaw/pitch convention of the game camera, never the raw located head roll.
std::atomic_bool g_nativeTagCurrent{true};
std::atomic_bool g_nativeImageHold{};
// [LOOKAHEAD] Native stereo: the sample taken at present N is rendered by the
// game and reaches the headset at present N+depth (measured from the exact
// FIFO match: selected serial vs latest serial, steadily 2).
// Predicting the pose for N's display time therefore hands the compositor
// pixels that are `depth` frames behind the head; it corrects rotation by
// reprojection but position and perspective stay late.  Predict for the
// present that will actually show the pixels instead.  <0 = auto (measured
// depth), 0 = the old behaviour, otherwise a fixed number of frames.
// Default 0: look-ahead 1-2 adds jitter.
// Predicting 8-17 ms past the runtime's own target amplifies tracking
// noise on this runtime, and the tag cannot hide it.  Auto (-1) is kept
// as an option.
std::atomic<float> g_headLookaheadFrames{0.0f};
std::atomic<int> g_headPipelineDepth{0};
// [FRAMETIME] how long the runtime held the presenter in xrWaitFrame this present.
std::atomic<long long> g_lastXrWaitMicros{0};

void XrLog(const char* format, ...);
std::atomic_uint64_t g_headPoseSerial{};
std::atomic_uint64_t g_renderedHeadPoseSerial{};
std::atomic_bool g_resetHeadPoseCenter{true};
std::atomic_bool g_sixDofCameraEnabled{true};

float ClampUnit(float value)
{
    if (value < -1.0f)
        return -1.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

float WrapRadians(float value)
{
    while (value > kPi)
        value -= 2.0f * kPi;
    while (value < -kPi)
        value += 2.0f * kPi;
    return value;
}

void QuaternionYawPitchRoll(const XrQuaternionf& q, float& yaw, float& pitch,
    float& roll)
{
    const float sinYaw = 2.0f * (q.w * q.y + q.x * q.z);
    const float cosYaw = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
    yaw = std::atan2(sinYaw, cosYaw);
    const float sinPitch = 2.0f * (q.w * q.x - q.z * q.y);
    pitch = std::asin(ClampUnit(sinPitch));
    const float sinRoll = 2.0f * (q.w * q.z + q.x * q.y);
    const float cosRoll = 1.0f - 2.0f * (q.z * q.z + q.x * q.x);
    roll = std::atan2(sinRoll, cosRoll);
}

void InvalidateHeadPose(bool requestCenter)
{
    AcquireSRWLockExclusive(&g_headPose.lock);
    g_headPose.valid = false;
    g_headCameraHistory.Clear();
    g_headFrameQueue.Clear();
    ++g_headPoseEpoch;
    if (requestCenter)
        g_headPose.centerValid = false;
    ReleaseSRWLockExclusive(&g_headPose.lock);
    g_renderedHeadPoseSerial.store(0, std::memory_order_release);
    if (requestCenter)
        g_resetHeadPoseCenter.store(true, std::memory_order_release);
}

void PublishHeadPose(const std::vector<XrView>& views, XrViewStateFlags flags, bool nativeStereo)
{
    if (views.size() < 2 || !(flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
    {
        InvalidateHeadPose(false);
        return;
    }

    float rawYaw = 0.0f;
    float rawPitch = 0.0f;
    float rawRoll = 0.0f;
    QuaternionYawPitchRoll(
        views[0].pose.orientation, rawYaw, rawPitch, rawRoll);
    const XrVector3f currentCenter{
        (views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
        (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
        (views[0].pose.position.z + views[1].pose.position.z) * 0.5f,
    };

    AcquireSRWLockExclusive(&g_headPose.lock);
    if (g_resetHeadPoseCenter.exchange(false, std::memory_order_acq_rel) ||
        !g_headPose.centerValid)
    {
        g_headCameraHistory.Clear();
        g_headFrameQueue.Clear();
        ++g_headPoseEpoch;
        g_headPose.centerYaw = rawYaw;
        g_headPose.centerPitch = rawPitch;
        g_headPose.centerRoll = rawRoll;
        g_headPose.centerPosition = currentCenter;
        g_headPose.centerValid = true;
    }
    g_headPose.yaw = WrapRadians(rawYaw - g_headPose.centerYaw);
    g_headPose.pitch = rawPitch - g_headPose.centerPitch;
    g_headPose.roll = WrapRadians(rawRoll - g_headPose.centerRoll);
    const bool positionValid =
        (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    const float dx = currentCenter.x - g_headPose.centerPosition.x;
    const float dy = currentCenter.y - g_headPose.centerPosition.y;
    const float dz = currentCenter.z - g_headPose.centerPosition.z;
    const float centerCos = std::cos(g_headPose.centerYaw);
    const float centerSin = std::sin(g_headPose.centerYaw);
    g_headPose.right = positionValid
        ? centerCos * dx - centerSin * dz : 0.0f;
    g_headPose.up = positionValid ? dy : 0.0f;
    g_headPose.forward = positionValid
        ? -centerSin * dx - centerCos * dz : 0.0f;
    const bool keepPosition =
        g_sixDofCameraEnabled.load(std::memory_order_acquire);
    for (std::size_t eye = 0; eye < 2; ++eye)
    {
        g_headPose.poses[eye] = views[eye].pose;
        // The native engine applies CENTERED yaw/pitch with zero local roll.
        // Compose that applied rotation with the saved tracking origin; raw
        // absolute yaw/pitch is not equivalent when the center is pitched.
        // Keep the accepted sampling, commit, and fallback timing unchanged.
        // [AERROLL] Every mode drives the game camera from ONE site:
        //   ApplyHeadRotation(matrix, -headYaw, headPitch, 0.0f)
        // with the CENTRED yaw/pitch and hard zero roll.  The tag has to
        // express that same rotation, so the construction is the same in AER
        // as in native stereo.  Absolute yaw/pitch (the old non-native branch)
        // is not equivalent once the recentre origin is anything but identity,
        // and it is never equivalent in roll.
        g_headPose.poses[eye].orientation = HeadPoseMath::CenteredCameraOrientation(
            rawYaw, rawPitch, g_headPose.centerYaw, g_headPose.centerPitch);
        if (!keepPosition)
        {
            g_headPose.poses[eye].position.x = g_headPose.centerPosition.x +
                (views[eye].pose.position.x - currentCenter.x);
            g_headPose.poses[eye].position.y = g_headPose.centerPosition.y +
                (views[eye].pose.position.y - currentCenter.y);
            g_headPose.poses[eye].position.z = g_headPose.centerPosition.z +
                (views[eye].pose.position.z - currentCenter.z);
        }
    }
    g_headPose.serial = g_headPoseSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_headPose.valid = true;
    RenderedHeadPose sample{};
    sample.poses[0] = g_headPose.poses[0];
    sample.poses[1] = g_headPose.poses[1];
    sample.serial = g_headPose.serial;
    g_headCameraHistory.Publish(sample);
    ReleaseSRWLockExclusive(&g_headPose.lock);
}

bool SnapshotRenderedHeadPose(RenderedHeadPose& output, bool nativeStereo)
{
    AcquireSRWLockShared(&g_headPose.lock);
    const auto rendered = g_renderedHeadPoseSerial.load(std::memory_order_acquire);
    const auto latest = g_headPose.serial;
    bool exact = nativeStereo && g_headPose.valid &&
        g_headFrameLatch.Snapshot(g_headPoseEpoch, output);
    // [LOOKAHEAD] the measured pipeline depth: how many samples newer than
    // the one these pixels were rendered with have been taken since.
    // Use the most common gap over the last 64 presents, not this frame's
    // gap: the raw value hops 1<->2 and every hop would move the prediction
    // target by a whole frame of head motion (its own judder on turns).
    // Present thread only, so plain statics.
    if (exact && latest >= output.serial)
    {
        static std::array<std::uint8_t, 64> ring{};
        static std::array<std::uint32_t, 5> counts{};
        static std::size_t index{}, filled{};
        const auto gap = static_cast<std::uint8_t>(
            std::min<std::uint64_t>(latest - output.serial, 4));
        if (filled == ring.size())
            --counts[ring[index]];
        else
            ++filled;
        ring[index] = gap;
        ++counts[gap];
        index = (index + 1) % ring.size();
        int mode = 0;
        for (int value = 1; value < 5; ++value)
            if (counts[value] > counts[mode])
                mode = value;
        g_headPipelineDepth.store(mode, std::memory_order_relaxed);
    }
    const float timing = g_nativeHeadTiming.load(std::memory_order_relaxed);
    bool timingApplied = false;
    if (exact && timing != 0.0f)
    {
        RenderedHeadPose adjusted{};
        timingApplied = HeadPoseTiming::AdjustOrientation(
            g_headCameraHistory, output, timing, adjusted);
        if (timingApplied) output = adjusted;
    }
    bool valid = exact;
    if (!valid && g_headPose.valid && rendered && latest == rendered)
    {
        output.poses[0] = g_headPose.poses[0];
        output.poses[1] = g_headPose.poses[1];
        output.serial = rendered;
        valid = true;
    }
    // [AERROLL] AER and mono have no draw-bound association to wait for, and a
    // missed serial match must not fall through to the caller's raw located
    // pose.  The latest published sample is the same roll-free construction
    // the camera writer used; taking it always is both correct and steadier.
    if (!valid && !nativeStereo && g_headPose.valid)
    {
        output.poses[0] = g_headPose.poses[0];
        output.poses[1] = g_headPose.poses[1];
        output.serial = g_headPose.serial;
        valid = true;
    }
    ReleaseSRWLockShared(&g_headPose.lock);
    if (nativeStereo)
    {
        const auto selected = (exact ? g_headPoseExact : g_headPoseFallback)
            .fetch_add(1, std::memory_order_relaxed) + 1;
        if (selected <= 3 || selected % 300 == 0)
            XrLog("[HEADPOSE] baselineTag=%s route=%s selected=%llu latest=%llu legacy=%llu "
                "exact/fallback=%llu/%llu uploadMatch/miss=%llu/%llu commitMiss=%llu "
                "uploadThread=%u presentThread=%u queueSeq=%llu queueDepth=%llu queueAgeMs=%llu queueDrops=%llu timing=%+.2f timingApplied=%u",
                g_nativeTagCurrent.load(std::memory_order_relaxed) ? "current" : "rendered",
                timingApplied ? "timing-calibrated" :
                    (exact ? "render-bank" : (valid ? "legacy" : "locate-fallback")),
                static_cast<unsigned long long>(valid ? output.serial : 0),
                static_cast<unsigned long long>(latest),
                static_cast<unsigned long long>(rendered),
                static_cast<unsigned long long>(g_headPoseExact.load()),
                static_cast<unsigned long long>(g_headPoseFallback.load()),
                static_cast<unsigned long long>(g_headCameraMatch.load()),
                static_cast<unsigned long long>(g_headCameraMiss.load()),
                static_cast<unsigned long long>(g_headCameraCommitMiss.load()),
                g_headCameraUploadThread.load(), GetCurrentThreadId(),
                static_cast<unsigned long long>(g_headFrameQueueSequence),
                static_cast<unsigned long long>(g_headFrameQueueDepth),
                static_cast<unsigned long long>(g_headFrameQueueAge),
                static_cast<unsigned long long>(g_headQueueDrops.load()),
                timing, timingApplied ? 1u : 0u);
    }
    return valid;
}

template <typename T>
void SafeRelease(T*& value)
{
    if (value)
    {
        value->Release();
        value = nullptr;
    }
}

void XrLog(const char* format, ...)
{
    if(QuietGameplayLog(format))return;
    char message[1536]{};
    va_list args;
    va_start(args, format);
    const HRESULT result = StringCchVPrintfA(message, _countof(message), format, args);
    va_end(args);
    if (SUCCEEDED(result))
    {
        char line[1600]{};
        if (SUCCEEDED(StringCchPrintfA(line, _countof(line), "[OPENXR] %s", message)))
            RetailLogLine(line);
    }
}

struct EyeSwapchain
{
    XrSwapchain handle{XR_NULL_HANDLE};
    std::int32_t width{};
    std::int32_t height{};
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> renderTargets;
};

// [SPLIT] native stereo presentation controls
std::atomic_bool g_splitSwap{};
std::atomic<float> g_splitConvergence{0.0f};
std::atomic_bool g_splitHonestLens{true};
std::atomic<float> g_splitFill{1.0f};
std::atomic_bool g_splitMonoFallback{};
// [SHARPEN] 0 = off (plain copy), 1 = strongest.
std::atomic<float> g_sharpenStrength{0.0f};
// [VIDEOFIT] Pre-rendered videos, loading screens and other frames with no 3D
// scene: the engine builds no stereo eye pair, the full-size per-eye capture
// has nothing to replay, and cropping half the backbuffer under the 3D lens
// claim (~118x87 degrees) would show a zoomed-in video.  After
// three presents without an eye build, both eyes get the region the game
// actually drew into, as a flat screen of a fixed angular width at its own
// aspect ratio, head-locked at infinity.  The first eye build ends it.
std::atomic_uint32_t g_flatPresents{};
constexpr std::uint32_t kVideoEnterPresents = 3;
std::atomic<float> g_videoScreenFov{0.0f};
float VideoScreenFovH()
{
    const float live = g_videoScreenFov.load(std::memory_order_relaxed);
    if (live > 0.0f) return live;
    static const float degrees = [] {
        wchar_t path[MAX_PATH]{};
        const auto n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = n ? wcsrchr(path, L'\\') : nullptr;
        if (!slash) return 70.0f;
        wcscpy_s(slash + 1, MAX_PATH - static_cast<std::size_t>(slash + 1 - path), L"ffxv-vr.ini");
        const int value = static_cast<int>(GetPrivateProfileIntW(L"Video", L"ScreenFovH", 70, path));
        return static_cast<float>(std::clamp(value, 30, 110));
    }();
    g_videoScreenFov.store(degrees, std::memory_order_relaxed);
    return degrees;
}

// [HUDLAYER] fed from the draw hook each present; read by the presenter.
std::atomic<ID3D11Texture2D*> g_hudSource{};
std::atomic_bool g_hudHasContent{};
std::atomic<ID3D11Texture2D*> g_objectiveSources[ObjectiveWorld::MaxMarkers]{};
std::atomic_bool g_objectiveHasContents[ObjectiveWorld::MaxMarkers]{};
ObjectiveWorld::Frame g_objectiveFrames[ObjectiveWorld::MaxMarkers]{};
std::atomic<ID3D11Texture2D*> g_targetSource{};
std::atomic_bool g_targetHasContent{};
ObjectiveWorld::Frame g_targetFrame{};
std::atomic<ID3D11Texture2D*> g_interactionSource{};
std::atomic_bool g_interactionHasContent{};
// [MENUQUAD] the Insert menu on its own head-locked panel (fixed
// distance, fixed width), showing only the menu window's
// rectangle of the menu texture so its pixels are not shrunk with the HUD.
std::atomic<ID3D11Texture2D*> g_menuSource{};
std::atomic_bool g_menuVisible{};
std::atomic<float> g_menuWidth{0.70f};   // metres at 1.2 m (0.70 = the menu window)
std::atomic<std::int32_t> g_menuRect[4]{};   // x, y, w, h in the menu texture
ObjectiveWorld::Frame g_interactionFrame{};
std::atomic<ID3D11Texture2D*> g_hudWorldSource{};
std::atomic_bool g_hudWorldHasContent{};
std::atomic_bool g_hudLayerEnabled{true};
std::atomic<float> g_hudDistance{1.5f};
// [UIWORLD] distance of the single world-marker quad.  Far, so its disparity
// is near zero and a marker fuses at (roughly) its target's depth for any
// target distance.  ONE quad for both eyes: per-eye quads showed
// every lens-layer element doubled with an IPD-sized offset - the runtime
// (VDXR) evidently composites per-eye quads into both eyes regardless of
// eyeVisibility, so per-eye quads are out.
std::atomic<float> g_hudWorldDistance{20.0f};
// [UILENS] fed from the draw hook each present: the lens the game's interface
// projects world markers with (stock camera, ~76.7 x 48 deg), which is NOT the
// lens the eye images are rendered with (pinned 87 deg).  Sizing the marker
// quad to the rendered lens put every marker ~2.1x too far from centre, which
// read as "anchored but drifting with head tracking".
std::atomic<float> g_uiLensP00{}, g_uiLensP11{};
// [UIBASE] Observed: markers can still move WITH head tracking.
// A marker that follows the head means its position in the interface texture
// does not change when the head turns: the game projects markers with its
// BASE (controller) camera, without the head rotation the mod adds to the
// rendered view.  The base camera sits at YawPitch(centerYaw, centerPitch)
// in tracking space (the game applies rawYaw-centerYaw / rawPitch-centerPitch
// on top of it - see CenteredCameraOrientation), so the marker quad is posed
// THERE and stays put when the head turns.  Toggle keeps the head-posed
// variant for comparison.
std::atomic_bool g_hudWorldBaseCamera{false}; // default HEAD pose (correct lens; markers follow the enemies)
std::atomic<float> g_hudWidth{1.6f};
std::atomic<float> g_hudYOffset{0.0f};
// [HUDLENS] Place the interface on the lens the game rendered, per eye, at
// that eye's RENDERED pose, so world-anchored elements (lock-on reticle,
// objective marker, enemy bars) land on their targets.  Measured:
// the fixed 1.6 m quad spans ~56 degrees while the rendered lens is
// 118.7 x 87 degrees, so every marker sat ~3x too close to the centre and,
// in VIEW space, slid with the head.
std::atomic_bool g_hudLensExact{true}; // now: world MARKERS on the lens (panel keeps the HUD)

XrVector3f RotateByQuaternion(const XrQuaternionf& q, const XrVector3f& v)
{
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

struct UvConstants
{
    float scaleX;
    float scaleY;
    float offsetX;
    float offsetY;
};

bool MatchGpuCamera(const float* view, GpuPoseTrace::Match& result)
{
    RenderedHeadPose pose{};
    AcquireSRWLockShared(&g_headPose.lock);
    const bool found = g_headPose.valid && g_headCameraHistory.Find(view, GetTickCount64(), pose);
    if (found) result = {pose.serial, g_headPoseEpoch, pose.poses[0].orientation};
    ReleaseSRWLockShared(&g_headPose.lock);
    return found;
}

XrPosef NativeLocatedPose(XrPosef eye, const XrQuaternionf& head)
{
    float yaw{},pitch{},roll{};
    QuaternionYawPitchRoll(head,yaw,pitch,roll);
    AcquireSRWLockShared(&g_headPose.lock);
    const bool valid=g_headPose.centerValid;
    const float centerYaw=g_headPose.centerYaw,centerPitch=g_headPose.centerPitch;
    ReleaseSRWLockShared(&g_headPose.lock);
    eye.orientation=valid
        ? HeadPoseMath::CenteredCameraOrientation(yaw,pitch,centerYaw,centerPitch)
        : HeadPoseMath::YawPitch(yaw,pitch);
    return eye;
}

bool MatchDrawCamera(const float* view, NativeDrawPose::Pose& result)
{
    RenderedHeadPose pose{};
    AcquireSRWLockShared(&g_headPose.lock);
    const bool found=g_headPose.valid && g_headCameraHistory.Find(view,GetTickCount64(),pose);
    if(found){result.eyes[0]=pose.poses[0];result.eyes[1]=pose.poses[1];
        result.serial=pose.serial;result.epoch=g_headPoseEpoch;
        result.renderedEye=pose.renderedEye;
        std::memcpy(result.view,view,sizeof(result.view));}
    ReleaseSRWLockShared(&g_headPose.lock);
    return found;
}

class Presenter
{
    struct HudBridge
    {
        const char* name{"panel"};
        ID3D11Texture2D* sharedGame{};
        IDXGIKeyedMutex* mutexGame{};
        ID3D11Texture2D* sharedXr{};
        IDXGIKeyedMutex* mutexXr{};
        HANDLE handle{};
        bool handleIsNt{};
        bool ready{};
        bool failed{};
        UINT w{};
        UINT h{};
        DXGI_FORMAT fmt{DXGI_FORMAT_UNKNOWN};
        XrSwapchain swapchain{XR_NULL_HANDLE};
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::uint64_t submits{};
        std::uint64_t copyFailures{};
    };

public:
    void OnPresent(IDXGISwapChain* swapChain, bool nativeStereoEnabled, bool aerEnabled)
    {
        PresentTiming::Scope timingScope(PresentTiming::XrTotal);
        if (!swapChain)
            return;

        if (!ready_.load(std::memory_order_acquire))
        {
            const std::uint64_t now = GetTickCount64();
            if (now < nextRetryMs_.load(std::memory_order_acquire))
                return;

            bool expected = false;
            if (!initializationStarted_.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel))
                return;

            XrLog("starting verified FFXV synchronous first-Present OpenXR bootstrap");
            if (!Initialize(swapChain))
            {
                XrLog("synchronous FFXV OpenXR bootstrap failed; retrying in %llu ms",
                    static_cast<unsigned long long>(kRetryDelayMs));
                Shutdown();
                nextRetryMs_.store(now + kRetryDelayMs, std::memory_order_release);
                initializationStarted_.store(false, std::memory_order_release);
                return;
            }
            ready_.store(true, std::memory_order_release);
            XrLog("verified synchronous FFXV OpenXR bootstrap ready");
        }

        PollEvents();
        if (!sessionRunning_)
            return;
        RenderFrame(swapChain, nativeStereoEnabled, aerEnabled);
    }

    void Shutdown()
    {
        ready_.store(false, std::memory_order_release);
        InvalidateHeadPose(true);
        ReleaseD3DResources();
        ReleaseHudLayer(hud_);
        ReleaseHudLayer(hudWorld_);
        for(auto& b:objectives_)ReleaseHudLayer(b);
        ReleaseHudLayer(target_);
        ReleaseHudLayer(interaction_);
        ReleaseHudLayer(menu_);
        if (blackSwapchain_ && xrDestroySwapchain_)
            xrDestroySwapchain_(blackSwapchain_);
        blackSwapchain_ = XR_NULL_HANDLE;
        blackSceneTried_ = false;
        ReleaseMetaFit();
        runtimeEyeFovValid_ = false;

        for (auto& eye : eyes_)
        {
            for (auto*& target : eye.renderTargets)
                SafeRelease(target);
            eye.renderTargets.clear();
            eye.images.clear();
            if (eye.handle && xrDestroySwapchain_)
                xrDestroySwapchain_(eye.handle);
            eye.handle = XR_NULL_HANDLE;
        }

        if (localSpace_ && xrDestroySpace_)
            xrDestroySpace_(localSpace_);
        localSpace_ = XR_NULL_HANDLE;
        if (viewSpace_ && xrDestroySpace_)
            xrDestroySpace_(viewSpace_);
        viewSpace_ = XR_NULL_HANDLE;
        if (session_ && xrDestroySession_)
            xrDestroySession_(session_);
        session_ = XR_NULL_HANDLE;
        sessionRunning_ = false;
        sessionState_ = XR_SESSION_STATE_UNKNOWN;
        if (instance_ && xrDestroyInstance_)
            xrDestroyInstance_(instance_);
        instance_ = XR_NULL_HANDLE;
        systemId_ = XR_NULL_SYSTEM_ID;
        if (loader_ && !loaderPinned_)
            FreeLibrary(loader_);
        loader_ = nullptr;
        loaderPinned_ = false;
        ResetFunctions();
    }

private:
    template <typename T>
    bool LoadFunction(XrInstance instance, const char* name, T& output)
    {
        PFN_xrVoidFunction value{};
        const XrResult result = xrGetInstanceProcAddr_
            ? xrGetInstanceProcAddr_(instance, name, &value)
            : XR_ERROR_FUNCTION_UNSUPPORTED;
        output = reinterpret_cast<T>(value);
        if (XR_FAILED(result) || !output)
        {
            XrLog("missing function %s result=%d", name, result);
            return false;
        }
        return true;
    }

    bool LoadGlobalFunctions()
    {
        xrGetInstanceProcAddr_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
            GetProcAddress(loader_, "xrGetInstanceProcAddr"));
        return xrGetInstanceProcAddr_ &&
            LoadFunction(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                xrEnumerateInstanceExtensionProperties_) &&
            LoadFunction(XR_NULL_HANDLE, "xrCreateInstance", xrCreateInstance_);
    }

    bool LoadInstanceFunctions()
    {
        return LoadFunction(instance_, "xrDestroyInstance", xrDestroyInstance_) &&
            LoadFunction(instance_, "xrGetSystem", xrGetSystem_) &&
            LoadFunction(instance_, "xrGetD3D11GraphicsRequirementsKHR", xrGetD3D11GraphicsRequirementsKHR_) &&
            LoadFunction(instance_, "xrCreateSession", xrCreateSession_) &&
            LoadFunction(instance_, "xrDestroySession", xrDestroySession_) &&
            LoadFunction(instance_, "xrCreateReferenceSpace", xrCreateReferenceSpace_) &&
            LoadFunction(instance_, "xrDestroySpace", xrDestroySpace_) &&
            LoadFunction(instance_, "xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViews_) &&
            LoadFunction(instance_, "xrEnumerateSwapchainFormats", xrEnumerateSwapchainFormats_) &&
            LoadFunction(instance_, "xrCreateSwapchain", xrCreateSwapchain_) &&
            LoadFunction(instance_, "xrDestroySwapchain", xrDestroySwapchain_) &&
            LoadFunction(instance_, "xrEnumerateSwapchainImages", xrEnumerateSwapchainImages_) &&
            LoadFunction(instance_, "xrPollEvent", xrPollEvent_) &&
            LoadFunction(instance_, "xrBeginSession", xrBeginSession_) &&
            LoadFunction(instance_, "xrEndSession", xrEndSession_) &&
            LoadFunction(instance_, "xrWaitFrame", xrWaitFrame_) &&
            LoadFunction(instance_, "xrBeginFrame", xrBeginFrame_) &&
            LoadFunction(instance_, "xrLocateViews", xrLocateViews_) &&
            LoadFunction(instance_, "xrAcquireSwapchainImage", xrAcquireSwapchainImage_) &&
            LoadFunction(instance_, "xrWaitSwapchainImage", xrWaitSwapchainImage_) &&
            LoadFunction(instance_, "xrReleaseSwapchainImage", xrReleaseSwapchainImage_) &&
            LoadFunction(instance_, "xrEndFrame", xrEndFrame_);
    }

    // [XRRUNTIME] Which runtime is this? SteamVR needs three things the other
    // runtimes do not ([SVRDEVICE], [STEAMVRFOV], [STEAMVRSCENE]); each one is
    // gated on this flag so Virtual Desktop and Meta stay bit-identical.
    void DetectRuntime()
    {
        isSteamVr_ = false;
        PFN_xrGetInstanceProperties getProperties{};
        if (!LoadFunction(instance_, "xrGetInstanceProperties", getProperties))
            return;
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_FAILED(getProperties(instance_, &properties)))
            return;
        properties.runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
        const auto runtime = RuntimeCompatibility::Detect(properties.runtimeName);
        isSteamVr_ = runtime.steamVr;
        isMeta_ = runtime.meta;
        XrLog("[XRRUNTIME] name='%s' version=%u.%u.%u steamvr=%d meta=%d", properties.runtimeName,
            XR_VERSION_MAJOR(properties.runtimeVersion),
            XR_VERSION_MINOR(properties.runtimeVersion),
            XR_VERSION_PATCH(properties.runtimeVersion), isSteamVr_ ? 1 : 0, isMeta_ ? 1 : 0);
    }

    // ---- [METAFIT] ---------------------------------------------------------
    // Meta's PC runtime IGNORES the FOV declared on a projection view: it maps
    // whatever rect it is given onto its OWN per-eye frustum (Quest 3 about
    // -54/+40 deg wide, +-43.5 tall). FFXV declares the game's lens instead
    // (stereo about +-48 x +-32), so on Meta each eye would be stretched,
    // differently per eye = a double image.
    // Cropping can only trim, and the game's picture is SHORTER than the lens,
    // so instead each eye is re-placed into a canvas shaped exactly like Meta's
    // frustum, at the same pixel density, black where the game drew nothing,
    // and that canvas is declared at Meta's own FOV. Declared == pixels ==
    // Meta's assumption, and the picture sits where Virtual Desktop puts it.
    // The eye images are shadowed while still acquired (a released swapchain
    // image must not be read), so a retained AER eye keeps its own pixels.
    struct MetaCanvas
    {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::int32_t width{};
        std::int32_t height{};
        std::vector<ID3D11Texture2D*> images;
        std::vector<ID3D11RenderTargetView*> targets;
        ID3D11Texture2D* shadow{};
        ID3D11ShaderResourceView* shadowView{};
        bool shadowValid{};
    };
    std::array<MetaCanvas, 2> metaCanvas_{};
    ID3D11VertexShader* metaVertexShader_{};
    ID3D11PixelShader* metaPixelShader_{};
    ID3D11Buffer* metaConstants_{};
    ID3D11SamplerState* metaSampler_{};
    bool metaFailed_{};
    std::uint64_t metaFrames_{};
    std::array<std::uint32_t, 2> viewMaxWidth_{};
    std::array<std::uint32_t, 2> viewMaxHeight_{};

    void ReleaseMetaFit()
    {
        for (auto& canvas : metaCanvas_)
        {
            for (auto*& target : canvas.targets)
                SafeRelease(target);
            canvas.targets.clear();
            canvas.images.clear();
            if (canvas.handle && xrDestroySwapchain_)
                xrDestroySwapchain_(canvas.handle);
            canvas.handle = XR_NULL_HANDLE;
            SafeRelease(canvas.shadowView);
            SafeRelease(canvas.shadow);
            canvas.shadowValid = false;
            canvas.width = canvas.height = 0;
        }
        SafeRelease(metaVertexShader_);
        SafeRelease(metaPixelShader_);
        SafeRelease(metaConstants_);
        SafeRelease(metaSampler_);
        metaFailed_ = false;
    }

    // Called in RenderEyes while the eye image is still acquired.
    void ShadowEyeForMeta(std::size_t eyeIndex, ID3D11Texture2D* eyeImage)
    {
        if (!isMeta_ || metaFailed_ || !eyeImage || eyeIndex >= metaCanvas_.size())
            return;
        auto& canvas = metaCanvas_[eyeIndex];
        if (!canvas.shadow)
        {
            D3D11_TEXTURE2D_DESC desc{};
            eyeImage->GetDesc(&desc);
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = 0;
            desc.CPUAccessFlags = 0;
            desc.Usage = D3D11_USAGE_DEFAULT;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            viewDesc.Format = ConcreteRenderTargetFormat(desc.Format,
                static_cast<DXGI_FORMAT>(swapchainFormat_));
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &canvas.shadow)) ||
                FAILED(device_->CreateShaderResourceView(canvas.shadow, &viewDesc, &canvas.shadowView)))
            {
                XrLog("[METAFIT] eye %zu shadow texture failed; raw submit kept", eyeIndex);
                SafeRelease(canvas.shadow);
                metaFailed_ = true;
                return;
            }
        }
        immediateContext_->CopyResource(canvas.shadow, eyeImage);
        canvas.shadowValid = true;
    }

    bool EnsureMetaShaders()
    {
        if (metaVertexShader_ && metaPixelShader_ && metaConstants_ && metaSampler_)
            return true;
        static constexpr char source[] = R"(
            cbuffer Fit : register(b0) { float4 uvTransform; };
            Texture2D eyeImage : register(t0);
            SamplerState linearClamp : register(s0);
            struct VOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
            VOut VSFit(uint id : SV_VertexID) {
                VOut o;
                const float2 uv = float2((id << 1) & 2, id & 2);
                o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
                o.uv = uv;
                return o;
            }
            float4 PSFit(VOut i) : SV_Target {
                return eyeImage.Sample(linearClamp, i.uv * uvTransform.xy + uvTransform.zw);
            }
        )";
        ID3DBlob* vertexBlob{};
        ID3DBlob* pixelBlob{};
        bool ok = CompileShader(source, "VSFit", "vs_5_0", &vertexBlob) &&
            CompileShader(source, "PSFit", "ps_5_0", &pixelBlob) &&
            SUCCEEDED(device_->CreateVertexShader(vertexBlob->GetBufferPointer(),
                vertexBlob->GetBufferSize(), nullptr, &metaVertexShader_)) &&
            SUCCEEDED(device_->CreatePixelShader(pixelBlob->GetBufferPointer(),
                pixelBlob->GetBufferSize(), nullptr, &metaPixelShader_));
        SafeRelease(vertexBlob);
        SafeRelease(pixelBlob);
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = 16;
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        ok = ok && SUCCEEDED(device_->CreateBuffer(&bufferDesc, nullptr, &metaConstants_)) &&
            SUCCEEDED(device_->CreateSamplerState(&samplerDesc, &metaSampler_));
        if (!ok)
        {
            XrLog("[METAFIT] shader setup failed; raw submit kept");
            metaFailed_ = true;
        }
        return ok;
    }

    bool EnsureMetaCanvas(std::size_t eyeIndex)
    {
        auto& canvas = metaCanvas_[eyeIndex];
        if (canvas.handle)
            return true;
        const XrFovf& f = runtimeEyeFov_[eyeIndex];
        const float tanW = std::tan(f.angleRight) - std::tan(f.angleLeft);
        const float tanH = std::tan(f.angleUp) - std::tan(f.angleDown);
        if (tanW <= 0.1f || tanH <= 0.1f)
            return false;
        // Same horizontal density as the game's eye image across the lens.
        std::int32_t width = eyes_[eyeIndex].width;
        std::int32_t height = static_cast<std::int32_t>(std::lround(width * tanH / tanW));
        const float maxW = static_cast<float>(viewMaxWidth_[eyeIndex]);
        const float maxH = static_cast<float>(viewMaxHeight_[eyeIndex]);
        if (maxW > 0.0f && maxH > 0.0f && (width > maxW || height > maxH))
        {
            const float shrink = std::min(maxW / width, maxH / height);
            width = static_cast<std::int32_t>(width * shrink);
            height = static_cast<std::int32_t>(height * shrink);
        }
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = swapchainFormat_;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain_(session_, &info, &canvas.handle)))
        {
            canvas.handle = XR_NULL_HANDLE;
            XrLog("[METAFIT] eye %zu canvas swapchain %dx%d failed; raw submit kept", eyeIndex, width, height);
            metaFailed_ = true;
            return false;
        }
        std::uint32_t count{};
        xrEnumerateSwapchainImages_(canvas.handle, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (!count || XR_FAILED(xrEnumerateSwapchainImages_(canvas.handle, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
        {
            metaFailed_ = true;
            return false;
        }
        for (const auto& image : images)
        {
            D3D11_TEXTURE2D_DESC desc{};
            image.texture->GetDesc(&desc);
            D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
            targetDesc.Format = ConcreteRenderTargetFormat(desc.Format, static_cast<DXGI_FORMAT>(swapchainFormat_));
            targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* target{};
            if (FAILED(device_->CreateRenderTargetView(image.texture, &targetDesc, &target)) &&
                FAILED(device_->CreateRenderTargetView(image.texture, nullptr, &target)))
            {
                XrLog("[METAFIT] eye %zu canvas RTV failed; raw submit kept", eyeIndex);
                metaFailed_ = true;
                return false;
            }
            canvas.images.push_back(image.texture);
            canvas.targets.push_back(target);
        }
        canvas.width = width;
        canvas.height = height;
        XrLog("[METAFIT] eye %zu canvas %dx%d for runtime frustum L/R/U/D=%.1f/%.1f/%.1f/%.1f deg",
            eyeIndex, width, height, f.angleLeft * 57.2958f, f.angleRight * 57.2958f,
            f.angleUp * 57.2958f, f.angleDown * 57.2958f);
        return true;
    }

    // At the submit choke point: re-place every eye view that points at an
    // eye swapchain into its Meta-frustum canvas. Pose is untouched.
    void FitToMetaFrustum(std::array<XrCompositionLayerProjectionView, 2>& views)
    {
        if (metaFailed_ || !runtimeEyeFovValid_ || !EnsureMetaShaders())
            return;
        for (std::size_t eye = 0; eye < views.size(); ++eye)
        {
            auto& view = views[eye];
            auto& canvas = metaCanvas_[eye];
            if (view.subImage.swapchain != eyes_[eye].handle || !canvas.shadowValid ||
                !EnsureMetaCanvas(eye))
                continue;
            const XrFovf& f = runtimeEyeFov_[eye];
            const XrFovf c = view.fov;
            const float tanFL = std::tan(f.angleLeft), tanFU = std::tan(f.angleUp);
            const float density = canvas.width / (std::tan(f.angleRight) - tanFL);
            D3D11_VIEWPORT viewport{};
            viewport.TopLeftX = (std::tan(c.angleLeft) - tanFL) * density;
            viewport.TopLeftY = (tanFU - std::tan(c.angleUp)) * density;
            viewport.Width = (std::tan(c.angleRight) - std::tan(c.angleLeft)) * density;
            viewport.Height = (std::tan(c.angleUp) - std::tan(c.angleDown)) * density;
            viewport.MaxDepth = 1.0f;
            if (!(viewport.Width > 1.0f && viewport.Height > 1.0f && viewport.Width < 30000.0f &&
                    viewport.Height < 30000.0f && std::fabs(viewport.TopLeftX) < 30000.0f &&
                    std::fabs(viewport.TopLeftY) < 30000.0f))
                continue;
            const auto& rect = view.subImage.imageRect;
            const float texW = static_cast<float>(eyes_[eye].width);
            const float texH = static_cast<float>(eyes_[eye].height);
            const float uv[4] = {rect.extent.width / texW, rect.extent.height / texH,
                rect.offset.x / texW, rect.offset.y / texH};

            std::uint32_t index{};
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(xrAcquireSwapchainImage_(canvas.handle, &acquire, &index)))
                continue;
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            bool drawn = false;
            if (XR_SUCCEEDED(xrWaitSwapchainImage_(canvas.handle, &wait)) && index < canvas.targets.size())
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(immediateContext_->Map(metaConstants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    std::memcpy(mapped.pData, uv, sizeof(uv));
                    immediateContext_->Unmap(metaConstants_, 0);
                    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    immediateContext_->ClearRenderTargetView(canvas.targets[index], black);
                    immediateContext_->IASetInputLayout(nullptr);
                    immediateContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    immediateContext_->VSSetShader(metaVertexShader_, nullptr, 0);
                    immediateContext_->PSSetShader(metaPixelShader_, nullptr, 0);
                    immediateContext_->PSSetConstantBuffers(0, 1, &metaConstants_);
                    immediateContext_->PSSetShaderResources(0, 1, &canvas.shadowView);
                    immediateContext_->PSSetSamplers(0, 1, &metaSampler_);
                    immediateContext_->OMSetRenderTargets(1, &canvas.targets[index], nullptr);
                    immediateContext_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
                    immediateContext_->RSSetState(nullptr);
                    immediateContext_->RSSetViewports(1, &viewport);
                    immediateContext_->Draw(3, 0);
                    ID3D11ShaderResourceView* noView{};
                    ID3D11RenderTargetView* noTarget{};
                    immediateContext_->PSSetShaderResources(0, 1, &noView);
                    immediateContext_->OMSetRenderTargets(1, &noTarget, nullptr);
                    immediateContext_->Flush();
                    drawn = true;
                }
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage_(canvas.handle, &release);
            if (!drawn)
                continue;
            if ((++metaFrames_ % 1800) == 1)
                XrLog("[METAFIT] eye %zu claim L/R/U/D=%.1f/%.1f/%.1f/%.1f rect %dx%d+%d+%d -> canvas %dx%d "
                    "at %.0f,%.0f size %.0fx%.0f, declared = runtime frustum",
                    eye, c.angleLeft * 57.2958f, c.angleRight * 57.2958f, c.angleUp * 57.2958f,
                    c.angleDown * 57.2958f, rect.extent.width, rect.extent.height, rect.offset.x,
                    rect.offset.y, canvas.width, canvas.height, viewport.TopLeftX, viewport.TopLeftY,
                    viewport.Width, viewport.Height);
            view.subImage.swapchain = canvas.handle;
            view.subImage.imageRect.offset = {0, 0};
            view.subImage.imageRect.extent = {canvas.width, canvas.height};
            view.subImage.imageArrayIndex = 0;
            view.fov = f;
        }
    }

    // [SVRDEVICE] SteamVR's OpenXR client creates a 1x1 legacy keyed-mutex
    // "sync texture" on the session device to hand frames to its compositor.
    // A device created with an
    // explicit adapter + D3D_DRIVER_TYPE_UNKNOWN cannot create one
    // (E_INVALIDARG) while a null-adapter HARDWARE device on the same GPU can;
    // when it fails, every projection layer is rejected silently and the
    // headset sits in the SteamVR void while xrEndFrame still returns success.
    // This reproduces SteamVR's exact call so the log names the outcome.
    bool CanCreateSyncTexture(ID3D11Device* device, HRESULT* outResult)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ID3D11Texture2D* texture{};
        const HRESULT result = device->CreateTexture2D(&desc, nullptr, &texture);
        SafeRelease(texture);
        if (outResult)
            *outResult = result;
        return SUCCEEDED(result);
    }

    // [STEAMVRFOV] SteamVR voids a projection view declared wider than its own
    // per-eye frustum. Declare the INTERSECTION of the claim and the runtime's
    // frustum and shrink the submitted rect by exactly the same tan-space
    // amount, so the declared FOV still describes precisely the pixels handed
    // over (nothing is re-rendered). Where the claim is already inside the
    // frustum this is a no-op.
    void CropToRuntimeFrustum(std::array<XrCompositionLayerProjectionView, 2>& views)
    {
        if (!runtimeEyeFovValid_)
            return;
        for (std::size_t eye = 0; eye < views.size(); ++eye)
        {
            auto& view = views[eye];
            const XrFovf declared = view.fov;
            const XrFovf& runtime = runtimeEyeFov_[eye];
            const float left = std::max(declared.angleLeft, runtime.angleLeft);
            const float right = std::min(declared.angleRight, runtime.angleRight);
            const float up = std::min(declared.angleUp, runtime.angleUp);
            const float down = std::max(declared.angleDown, runtime.angleDown);
            if (left == declared.angleLeft && right == declared.angleRight &&
                up == declared.angleUp && down == declared.angleDown)
                continue;
            const float tanLeft = std::tan(declared.angleLeft);
            const float tanUp = std::tan(declared.angleUp);
            const float spanX = std::tan(declared.angleRight) - tanLeft;
            const float spanY = tanUp - std::tan(declared.angleDown);
            if (right - left <= 0.05f || up - down <= 0.05f || spanX <= 1e-4f || spanY <= 1e-4f)
                continue;
            const auto rect = view.subImage.imageRect;
            const float w = static_cast<float>(rect.extent.width);
            const float h = static_cast<float>(rect.extent.height);
            std::int32_t x0 = static_cast<std::int32_t>(std::lround((std::tan(left) - tanLeft) / spanX * w));
            std::int32_t x1 = static_cast<std::int32_t>(std::lround((std::tan(right) - tanLeft) / spanX * w));
            std::int32_t y0 = static_cast<std::int32_t>(std::lround((tanUp - std::tan(up)) / spanY * h));
            std::int32_t y1 = static_cast<std::int32_t>(std::lround((tanUp - std::tan(down)) / spanY * h));
            x0 = std::clamp(x0, 0, rect.extent.width - 1);
            x1 = std::clamp(x1, x0 + 1, rect.extent.width);
            y0 = std::clamp(y0, 0, rect.extent.height - 1);
            y1 = std::clamp(y1, y0 + 1, rect.extent.height);
            view.subImage.imageRect.offset = {rect.offset.x + x0, rect.offset.y + y0};
            view.subImage.imageRect.extent = {x1 - x0, y1 - y0};
            view.fov = {left, right, up, down};
            if ((++fovCropReports_ % 1200) == 1)
                XrLog("[STEAMVRFOV] eye %zu claim L/R/U/D=%.1f/%.1f/%.1f/%.1f cropped to runtime frustum "
                    "%.1f/%.1f/%.1f/%.1f deg, rect %dx%d+%d+%d -> %dx%d+%d+%d",
                    eye, declared.angleLeft * 57.2958f, declared.angleRight * 57.2958f,
                    declared.angleUp * 57.2958f, declared.angleDown * 57.2958f,
                    left * 57.2958f, right * 57.2958f, up * 57.2958f, down * 57.2958f,
                    rect.extent.width, rect.extent.height, rect.offset.x, rect.offset.y,
                    view.subImage.imageRect.extent.width, view.subImage.imageRect.extent.height,
                    view.subImage.imageRect.offset.x, view.subImage.imageRect.offset.y);
        }
    }

    // [STEAMVRSCENE] One tiny swapchain, cleared to opaque black once at
    // creation and never touched again: no per-frame cost.
    bool EnsureBlackScene()
    {
        if (blackSwapchain_)
            return true;
        if (blackSceneTried_)
            return false;
        blackSceneTried_ = true;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = swapchainFormat_;
        info.sampleCount = 1;
        info.width = kBlackSceneSize;
        info.height = kBlackSceneSize;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain_(session_, &info, &blackSwapchain_)))
        {
            blackSwapchain_ = XR_NULL_HANDLE;
            XrLog("[STEAMVRSCENE] black scene swapchain create FAILED");
            return false;
        }
        std::uint32_t count{};
        xrEnumerateSwapchainImages_(blackSwapchain_, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (!count || XR_FAILED(xrEnumerateSwapchainImages_(blackSwapchain_, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))))
        {
            xrDestroySwapchain_(blackSwapchain_);
            blackSwapchain_ = XR_NULL_HANDLE;
            return false;
        }
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (const auto& image : images)
        {
            ID3D11RenderTargetView* target{};
            if (image.texture &&
                SUCCEEDED(device_->CreateRenderTargetView(image.texture, nullptr, &target)) && target)
                immediateContext_->ClearRenderTargetView(target, black);
            SafeRelease(target);
        }
        immediateContext_->Flush();
        // One acquire/release so the runtime sees a chain with valid content.
        std::uint32_t index{};
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage_(blackSwapchain_, &acquire, &index)))
        {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage_(blackSwapchain_, &wait);
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage_(blackSwapchain_, &release);
        }
        XrLog("[STEAMVRSCENE] black scene layer ready (%dpx, SteamVR only)", kBlackSceneSize);
        return true;
    }

    bool HasD3D11Extension()
    {
        std::uint32_t count{};
        XrResult result = xrEnumerateInstanceExtensionProperties_(nullptr, 0, &count, nullptr);
        if (XR_FAILED(result) || count == 0)
            return false;
        std::vector<XrExtensionProperties> extensions(count);
        for (auto& extension : extensions)
            extension = {XR_TYPE_EXTENSION_PROPERTIES};
        result = xrEnumerateInstanceExtensionProperties_(nullptr, count, &count, extensions.data());
        if (XR_FAILED(result))
            return false;
        for (const auto& extension : extensions)
            if (std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
                return true;
        return false;
    }

    bool Initialize(IDXGISwapChain* swapChain)
    {
        XrLog("initializing synchronously from the verified first-Present path");
        loader_ = LoadLibraryW(L"openxr_loader.dll");
        if (!loader_)
        {
            XrLog("openxr_loader.dll unavailable error=%lu", GetLastError());
            return false;
        }
        if (!LoadGlobalFunctions() || !HasD3D11Extension())
        {
            XrLog("XR_KHR_D3D11_enable is unavailable");
            return false;
        }
        HMODULE pinned{};
        loaderPinned_ = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(xrGetInstanceProcAddr_), &pinned) != FALSE;
        XrLog("OpenXR loader pin=%d", loaderPinned_ ? 1 : 0);
        XrLog("loader ready; XR_KHR_D3D11_enable available");

        XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        StringCchCopyA(instanceInfo.applicationInfo.applicationName,
            _countof(instanceInfo.applicationInfo.applicationName), "FFXV Geo11 Stereo");
        instanceInfo.applicationInfo.applicationVersion = 1;
        StringCchCopyA(instanceInfo.applicationInfo.engineName,
            _countof(instanceInfo.applicationInfo.engineName), "FFXVVR");
        instanceInfo.applicationInfo.engineVersion = 1;
        instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.enabledExtensionNames = extensions;
        XrResult result = xrCreateInstance_(&instanceInfo, &instance_);
        if (XR_FAILED(result))
        {
            XrLog("xrCreateInstance failed result=%d", result);
            return false;
        }
        if (!LoadInstanceFunctions())
            return false;
        XrLog("instance created and instance functions loaded");
        DetectRuntime();

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        result = xrGetSystem_(instance_, &systemInfo, &systemId_);
        if (XR_FAILED(result))
        {
            XrLog("xrGetSystem failed result=%d", result);
            return false;
        }
        XrLog("HMD system acquired id=%llu", static_cast<unsigned long long>(systemId_));

        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device),
                reinterpret_cast<void**>(&gameDevice_))) || !gameDevice_)
        {
            XrLog("IDXGISwapChain::GetDevice failed");
            return false;
        }
        gameDevice_->GetImmediateContext(&gameImmediateContext_);
        ID3D11Texture2D* desktopBuffer{};
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&desktopBuffer))) && desktopBuffer)
        {
            D3D11_TEXTURE2D_DESC desktopDesc{};
            desktopBuffer->GetDesc(&desktopDesc);
            desktopFormat_ = desktopDesc.Format;
            desktopWidth_ = desktopDesc.Width;
            desktopHeight_ = desktopDesc.Height;
            XrLog("game D3D11 device ready; backbuffer=%ux%u format=%u",
                desktopDesc.Width, desktopDesc.Height, static_cast<unsigned>(desktopFormat_));
            desktopBuffer->Release();
        }
        else
        {
            XrLog("game D3D11 device ready; backbuffer description unavailable");
        }

        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        result = xrGetD3D11GraphicsRequirementsKHR_(instance_, systemId_, &requirements);
        if (XR_FAILED(result))
        {
            XrLog("xrGetD3D11GraphicsRequirementsKHR failed result=%d", result);
            return false;
        }
        if (!CreateCleanXrDevice(requirements.adapterLuid, requirements.minFeatureLevel))
            return false;
        XrLog("clean unwrapped D3D11 device accepted for OpenXR session binding");

        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device_;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.next = &binding;
        sessionInfo.systemId = systemId_;
        result = xrCreateSession_(instance_, &sessionInfo, &session_);
        if (XR_FAILED(result))
        {
            XrLog("xrCreateSession failed result=%d", result);
            return false;
        }
        XrLog("session created");

        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        result = xrCreateReferenceSpace_(session_, &spaceInfo, &viewSpace_);
        if (XR_FAILED(result))
        {
            XrLog("xrCreateReferenceSpace(VIEW) failed result=%d", result);
            return false;
        }
        XrLog("VIEW reference space created");

        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        result = xrCreateReferenceSpace_(session_, &spaceInfo, &localSpace_);
        if (XR_FAILED(result))
        {
            localSpace_ = XR_NULL_HANDLE;
            XrLog("xrCreateReferenceSpace(LOCAL) failed result=%d; head tracking disabled",
                result);
        }
        else
            XrLog("LOCAL reference space created for camera head tracking");

        if (!CreateEyeSwapchains())
            return false;
        if (!CreateCrossDeviceBridge())
            return false;

        XrLog("initialized: format=%lld eyes=%dx%d,%dx%d fovY=%.1f aspect=%.3f",
            static_cast<long long>(swapchainFormat_), eyes_[0].width, eyes_[0].height,
            eyes_[1].width, eyes_[1].height, kRenderedVerticalFovDegrees, kRenderedAspect);
        return true;
    }

    bool CreateCleanXrDevice(const LUID& adapterLuid, D3D_FEATURE_LEVEL minFeatureLevel)
    {
        wchar_t systemDirectory[MAX_PATH]{};
        if (!GetSystemDirectoryW(systemDirectory, MAX_PATH))
        {
            XrLog("GetSystemDirectoryW failed while creating clean XR device");
            return false;
        }

        wchar_t dxgiPath[MAX_PATH]{};
        wchar_t d3d11Path[MAX_PATH]{};
        if (FAILED(StringCchPrintfW(dxgiPath, _countof(dxgiPath), L"%s\\dxgi.dll",
                systemDirectory)) ||
            FAILED(StringCchPrintfW(d3d11Path, _countof(d3d11Path), L"%s\\d3d11.dll",
                systemDirectory)))
            return false;

        realDxgi_ = LoadLibraryW(dxgiPath);
        realD3D11_ = LoadLibraryW(d3d11Path);
        using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
        using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
            const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*,
            ID3D11DeviceContext**);
        auto createFactory = realDxgi_ ? reinterpret_cast<CreateFactory1Fn>(
            GetProcAddress(realDxgi_, "CreateDXGIFactory1")) : nullptr;
        auto createDevice = realD3D11_ ? reinterpret_cast<CreateDeviceFn>(
            GetProcAddress(realD3D11_, "D3D11CreateDevice")) : nullptr;
        if (!createFactory || !createDevice)
        {
            XrLog("could not resolve real system D3D11/DXGI exports");
            return false;
        }

        IDXGIFactory1* factory{};
        if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
            !factory)
        {
            XrLog("real CreateDXGIFactory1 failed");
            return false;
        }

        IDXGIAdapter1* chosen{};
        for (UINT index = 0;; ++index)
        {
            IDXGIAdapter1* candidate{};
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND)
                break;
            if (!candidate)
                continue;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
                std::memcmp(&desc.AdapterLuid, &adapterLuid, sizeof(LUID)) == 0)
            {
                chosen = candidate;
                break;
            }
            candidate->Release();
        }
        factory->Release();
        if (!chosen)
        {
            XrLog("runtime adapter LUID was not found by real DXGI");
            return false;
        }

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL createdLevel{};
        const HRESULT result = createDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            levels, static_cast<UINT>(_countof(levels)), D3D11_SDK_VERSION, &device_,
            &createdLevel, &immediateContext_);
        chosen->Release();
        if (FAILED(result) || !device_ || !immediateContext_)
        {
            XrLog("real D3D11CreateDevice(clean XR device) failed hr=0x%08X",
                static_cast<unsigned>(result));
            return false;
        }
        if (createdLevel < minFeatureLevel)
        {
            XrLog("clean XR device feature level 0x%X is below runtime minimum 0x%X",
                createdLevel, minFeatureLevel);
            return false;
        }
        XrLog("clean XR device created on runtime adapter featureLevel=0x%X", createdLevel);
        if (isSteamVr_)
        {
            // [SVRDEVICE] SteamVR only: swap to the recipe proven to
            // create the sync texture, provided the null adapter resolves
            // to the runtime's GPU. Every other runtime keeps the device above.
            HRESULT explicitProbe = S_OK;
            const bool explicitOk = CanCreateSyncTexture(device_, &explicitProbe);
            XrLog("[SVRDEVICE] explicit-adapter device sync texture hr=0x%08X %s",
                static_cast<unsigned>(explicitProbe), explicitOk ? "OK" : "FAIL");
            ID3D11Device* hardware{};
            ID3D11DeviceContext* hardwareContext{};
            D3D_FEATURE_LEVEL hardwareLevel{};
            const HRESULT hardwareResult = createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                0, levels, static_cast<UINT>(_countof(levels)), D3D11_SDK_VERSION, &hardware,
                &hardwareLevel, &hardwareContext);
            bool sameAdapter = false;
            if (SUCCEEDED(hardwareResult) && hardware)
            {
                IDXGIDevice* dxgiDevice{};
                IDXGIAdapter* adapter{};
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(hardware->QueryInterface(__uuidof(IDXGIDevice),
                        reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice &&
                    SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter &&
                    SUCCEEDED(adapter->GetDesc(&desc)))
                    sameAdapter = std::memcmp(&desc.AdapterLuid, &adapterLuid, sizeof(LUID)) == 0;
                SafeRelease(adapter);
                SafeRelease(dxgiDevice);
            }
            HRESULT hardwareProbe = E_FAIL;
            const bool hardwareOk = sameAdapter && hardwareLevel >= minFeatureLevel &&
                CanCreateSyncTexture(hardware, &hardwareProbe);
            XrLog("[SVRDEVICE] null-adapter HARDWARE device hr=0x%08X sameAdapter=%d "
                "featureLevel=0x%X sync texture hr=0x%08X %s",
                static_cast<unsigned>(hardwareResult), sameAdapter ? 1 : 0, hardwareLevel,
                static_cast<unsigned>(hardwareProbe), hardwareOk ? "OK" : "FAIL");
            if (hardwareOk)
            {
                SafeRelease(immediateContext_);
                SafeRelease(device_);
                device_ = hardware;
                immediateContext_ = hardwareContext;
                XrLog("[SVRDEVICE] SteamVR session bound to the null-adapter HARDWARE device");
            }
            else
            {
                SafeRelease(hardwareContext);
                SafeRelease(hardware);
                XrLog("[SVRDEVICE] keeping the explicit-adapter device%s", explicitOk ? ""
                    : " (WARNING: it cannot create SteamVR's sync texture; expect a blank world)");
            }
        }
        // [VRHINT] first (and only) time: the load-time notification counts
        // its 5 seconds from here, not from the F9 press.
        unsigned long long expected = 0;
        g_xrReadyTick.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_relaxed);
        return true;
    }

    bool CreateCrossDeviceBridge()
    {
        if (!gameDevice_ || !device_ || !desktopWidth_ || !desktopHeight_)
            return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desktopWidth_;
        desc.Height = desktopHeight_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = desktopFormat_;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        HRESULT result = gameDevice_->CreateTexture2D(&desc, nullptr, &sharedTextureGame_);
        bool legacy = false;
        if (FAILED(result) || !sharedTextureGame_)
        {
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            result = gameDevice_->CreateTexture2D(&desc, nullptr, &sharedTextureGame_);
            legacy = true;
        }
        if (FAILED(result) || !sharedTextureGame_)
        {
            XrLog("cross-device shared texture creation failed hr=0x%08X",
                static_cast<unsigned>(result));
            return false;
        }
        if (FAILED(sharedTextureGame_->QueryInterface(__uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&sharedMutexGame_))) || !sharedMutexGame_)
        {
            XrLog("game-side shared keyed mutex unavailable");
            return false;
        }

        if (legacy)
        {
            IDXGIResource* resource{};
            result = sharedTextureGame_->QueryInterface(__uuidof(IDXGIResource),
                reinterpret_cast<void**>(&resource));
            if (SUCCEEDED(result) && resource)
            {
                result = resource->GetSharedHandle(&sharedHandle_);
                resource->Release();
            }
            if (SUCCEEDED(result) && sharedHandle_)
                result = device_->OpenSharedResource(sharedHandle_, __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&sharedTextureXr_));
        }
        else
        {
            IDXGIResource1* resource{};
            result = sharedTextureGame_->QueryInterface(__uuidof(IDXGIResource1),
                reinterpret_cast<void**>(&resource));
            if (SUCCEEDED(result) && resource)
            {
                result = resource->CreateSharedHandle(nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                    &sharedHandle_);
                resource->Release();
            }
            ID3D11Device1* xrDevice1{};
            if (SUCCEEDED(result) && sharedHandle_ &&
                SUCCEEDED(device_->QueryInterface(__uuidof(ID3D11Device1),
                    reinterpret_cast<void**>(&xrDevice1))) && xrDevice1)
            {
                result = xrDevice1->OpenSharedResource1(sharedHandle_,
                    __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sharedTextureXr_));
                xrDevice1->Release();
                sharedHandleIsNt_ = true;
            }
        }
        if (FAILED(result) || !sharedTextureXr_ ||
            FAILED(sharedTextureXr_->QueryInterface(__uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&sharedMutexXr_))) || !sharedMutexXr_)
        {
            XrLog("XR-side shared texture open failed hr=0x%08X",
                static_cast<unsigned>(result));
            return false;
        }
        sharedReady_ = true;
        XrLog("geo-11 cross-device bridge ready %ux%u format=%u mode=%s",
            desktopWidth_, desktopHeight_, static_cast<unsigned>(desktopFormat_),
            legacy ? "legacy-keyed" : "nt-keyed");
        return true;
    }

    // ---- [HUDLAYER] --------------------------------------------------------
    void ReleaseHudLayer(HudBridge& b)
    {
        if (b.swapchain != XR_NULL_HANDLE && xrDestroySwapchain_)
            xrDestroySwapchain_(b.swapchain);
        b.swapchain = XR_NULL_HANDLE;
        b.images.clear();
        SafeRelease(b.mutexXr);
        SafeRelease(b.sharedXr);
        SafeRelease(b.mutexGame);
        SafeRelease(b.sharedGame);
        if (b.handle && b.handleIsNt)
            CloseHandle(b.handle);
        b.handle = nullptr;
        b.handleIsNt = false;
        b.ready = false;
        b.w = 0;
        b.h = 0;
        b.fmt = DXGI_FORMAT_UNKNOWN;
    }

    bool EnsureHudBridge(HudBridge& b, ID3D11Texture2D* source)
    {
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (b.ready && desc.Width == b.w && desc.Height == b.h &&
            desc.Format == b.fmt)
            return true;
        if (b.failed)
            return false;
        ReleaseHudLayer(b);
        if (!gameDevice_ || !device_ || session_ == XR_NULL_HANDLE)
            return false;
        bool offered = false;
        for (const auto format : offeredFormats_)
            if (format == static_cast<std::int64_t>(desc.Format))
                offered = true;
        if (!offered)
        {
            XrLog("[HUDLAYER] runtime offers no swapchain format %u; layer disabled",
                static_cast<unsigned>(desc.Format));
            b.failed = true;
            return false;
        }

        D3D11_TEXTURE2D_DESC shared{};
        shared.Width = desc.Width;
        shared.Height = desc.Height;
        shared.MipLevels = 1;
        shared.ArraySize = 1;
        shared.Format = desc.Format;
        shared.SampleDesc.Count = 1;
        shared.Usage = D3D11_USAGE_DEFAULT;
        shared.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        shared.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        HRESULT result = gameDevice_->CreateTexture2D(&shared, nullptr, &b.sharedGame);
        bool legacy = false;
        if (FAILED(result) || !b.sharedGame)
        {
            shared.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            result = gameDevice_->CreateTexture2D(&shared, nullptr, &b.sharedGame);
            legacy = true;
        }
        if (FAILED(result) || !b.sharedGame ||
            FAILED(b.sharedGame->QueryInterface(__uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&b.mutexGame))) || !b.mutexGame)
        {
            XrLog("[HUDLAYER] shared texture creation failed hr=0x%08X",
                static_cast<unsigned>(result));
            ReleaseHudLayer(b);
            b.failed = true;
            return false;
        }
        if (legacy)
        {
            IDXGIResource* resource{};
            result = b.sharedGame->QueryInterface(__uuidof(IDXGIResource),
                reinterpret_cast<void**>(&resource));
            if (SUCCEEDED(result) && resource)
            {
                result = resource->GetSharedHandle(&b.handle);
                resource->Release();
            }
            if (SUCCEEDED(result) && b.handle)
                result = device_->OpenSharedResource(b.handle, __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&b.sharedXr));
        }
        else
        {
            IDXGIResource1* resource{};
            result = b.sharedGame->QueryInterface(__uuidof(IDXGIResource1),
                reinterpret_cast<void**>(&resource));
            if (SUCCEEDED(result) && resource)
            {
                result = resource->CreateSharedHandle(nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                    &b.handle);
                resource->Release();
            }
            ID3D11Device1* xrDevice1{};
            if (SUCCEEDED(result) && b.handle &&
                SUCCEEDED(device_->QueryInterface(__uuidof(ID3D11Device1),
                    reinterpret_cast<void**>(&xrDevice1))) && xrDevice1)
            {
                result = xrDevice1->OpenSharedResource1(b.handle,
                    __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&b.sharedXr));
                xrDevice1->Release();
                b.handleIsNt = true;
            }
        }
        if (FAILED(result) || !b.sharedXr ||
            FAILED(b.sharedXr->QueryInterface(__uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&b.mutexXr))) || !b.mutexXr)
        {
            XrLog("[HUDLAYER] XR-side shared open failed hr=0x%08X",
                static_cast<unsigned>(result));
            ReleaseHudLayer(b);
            b.failed = true;
            return false;
        }

        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = static_cast<std::int64_t>(desc.Format);
        createInfo.sampleCount = 1;
        createInfo.width = desc.Width;
        createInfo.height = desc.Height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;
        XrResult xr = xrCreateSwapchain_(session_, &createInfo, &b.swapchain);
        std::uint32_t imageCount{};
        if (XR_SUCCEEDED(xr))
            xr = xrEnumerateSwapchainImages_(b.swapchain, 0, &imageCount, nullptr);
        if (XR_SUCCEEDED(xr) && imageCount)
        {
            b.images.resize(imageCount);
            for (auto& image : b.images)
                image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr = xrEnumerateSwapchainImages_(b.swapchain, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(b.images.data()));
        }
        if (XR_FAILED(xr) || !imageCount)
        {
            XrLog("[HUDLAYER] quad swapchain creation failed result=%d", xr);
            ReleaseHudLayer(b);
            b.failed = true;
            return false;
        }
        b.w = desc.Width;
        b.h = desc.Height;
        b.fmt = desc.Format;
        b.ready = true;
        XrLog("[HUDLAYER] bridge(%s) ready %ux%u fmt=%u mode=%s images=%u", b.name, b.w, b.h,
            static_cast<unsigned>(b.fmt), legacy ? "legacy-keyed" : "nt-keyed",
            imageCount);
        return true;
    }

    bool CopyHudFrame(HudBridge& b, ID3D11Texture2D* source)
    {
        if (&b==&objectives_[0]) objectivePixels_.Poll(immediateContext_,XrLog);
        if (PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return b.mutexGame->AcquireSync(0, 16);}) != S_OK)
            return false;
        gameImmediateContext_->CopyResource(b.sharedGame, source);
        if (FAILED(b.mutexGame->ReleaseSync(1)))
            return false;
        if (PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return b.mutexXr->AcquireSync(1, 16);}) != S_OK)
            return false;
        bool ok = false;
        std::uint32_t index{};
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage_(b.swapchain, &acquireInfo, &index)) &&
            index < b.images.size())
        {
            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(PresentTiming::Measure(PresentTiming::SwapWait,[&]{return xrWaitSwapchainImage_(b.swapchain, &waitInfo);})))
            {
                immediateContext_->CopyResource(b.images[index].texture, b.sharedXr);
                if (&b==&objectives_[0] && objectivePixels_.Enabled()) {
                    RenderedHeadPose camera{};
                    AcquireSRWLockShared(&g_headPose.lock);
                    const bool paired=g_headFrameLatch.Snapshot(g_headPoseEpoch,camera) && IsSyncCameraView(camera.markerView);
                    ReleaseSRWLockShared(&g_headPose.lock);
                    if(objectivePixels_.Wants(paired,b.submits+1))
                        objectivePixels_.Queue(immediateContext_,b.images[index].texture,b.submits+1,XrLog);
                }
                immediateContext_->Flush();
                ok = true;
            }
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage_(b.swapchain, &releaseInfo);
        }
        b.mutexXr->ReleaseSync(0);
        return ok;
    }

    struct HudTransferRequest
    {
        HudBridge* bridge;
        std::atomic<ID3D11Texture2D*>* source;
        std::atomic_bool* content;
        bool enabled;
        bool* ready;
    };

    template<std::size_t N>
    void PrepareHudBatch(const std::array<HudTransferRequest,N>& requests)
    {
        PresentTiming::Scope timingScope(PresentTiming::HudTotal);
        // Preserve the existing diagnostic path if explicitly enabled in a
        // research build. The playable candidate has pixel probes disabled.
        if(objectivePixels_.Enabled()) {
            for(const auto& r:requests)
                *r.ready=r.enabled && PrepareHudLayer(*r.bridge,*r.source,*r.content);
            return;
        }
        struct State { bool xrOwned{}, acquired{}, copied{}, attempted{}; };
        std::array<State,N> state{};
        RunTransferBatch<N>(
            [&](std::size_t i) {
                const auto& r=requests[i]; auto& b=*r.bridge;
                *r.ready=false;
                if(!r.enabled || !g_hudLayerEnabled.load(std::memory_order_relaxed)) return false;
                auto* source=r.source->load(std::memory_order_acquire);
                if(!source || !r.content->load(std::memory_order_relaxed) || !EnsureHudBridge(b,source)) return false;
                state[i].attempted=true;
                if(PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return b.mutexGame->AcquireSync(0,16);})!=S_OK) return false;
                gameImmediateContext_->CopyResource(b.sharedGame,source);
                return SUCCEEDED(b.mutexGame->ReleaseSync(1));
            },
            [&](std::size_t i) {
                auto& b=*requests[i].bridge; auto& s=state[i];
                if(PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return b.mutexXr->AcquireSync(1,16);})!=S_OK) return false;
                s.xrOwned=true;
                std::uint32_t index{};
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if(XR_FAILED(xrAcquireSwapchainImage_(b.swapchain,&acquire,&index))) return false;
                s.acquired=true;
                XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait.timeout=XR_INFINITE_DURATION;
                if(XR_FAILED(PresentTiming::Measure(PresentTiming::SwapWait,[&]{return xrWaitSwapchainImage_(b.swapchain,&wait);}))) return false;
                if(index>=b.images.size()) return false;
                immediateContext_->CopyResource(b.images[index].texture,b.sharedXr);
                s.copied=true;
                return true;
            },
            [&]{ immediateContext_->Flush(); },
            [&](std::size_t i) {
                auto& b=*requests[i].bridge; auto& s=state[i];
                if(s.acquired) {
                    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage_(b.swapchain,&release);
                }
                if(s.xrOwned) b.mutexXr->ReleaseSync(0);
                *requests[i].ready=s.copied;
            });
        for(std::size_t i=0;i<N;++i) {
            auto& b=*requests[i].bridge;
            if(*requests[i].ready) {
                ++b.submits;
                if(b.submits<=3 || (b.submits%1800)==0)
                    XrLog("[HUDLAYER] batched submission %llu",static_cast<unsigned long long>(b.submits));
            } else if(state[i].attempted && (++b.copyFailures%300)==1)
                XrLog("[HUDLAYER] batched copy failed (%llu so far)",static_cast<unsigned long long>(b.copyFailures));
        }
    }

    // True when this frame's quad layer holds a fresh interface image.
    bool PrepareHudLayer(HudBridge& b, std::atomic<ID3D11Texture2D*>& sourceSlot, std::atomic_bool& contentSlot)
    {
        PresentTiming::Scope timingScope(PresentTiming::HudTotal);
        if (!g_hudLayerEnabled.load(std::memory_order_relaxed))
            return false;
        auto* source = sourceSlot.load(std::memory_order_acquire);
        if (!source || !contentSlot.load(std::memory_order_relaxed))
            return false;
        if (!EnsureHudBridge(b, source))
            return false;
        const bool ok = CopyHudFrame(b, source);
        if (ok)
        {
            ++b.submits;
            if (b.submits <= 3 || (b.submits % 1800) == 0)
                XrLog("[HUDLAYER] submitted %llu distance=%.2fm width=%.2fm yOffset=%.2fm",
                    static_cast<unsigned long long>(b.submits),
                    g_hudDistance.load(std::memory_order_relaxed),
                    g_hudWidth.load(std::memory_order_relaxed),
                    g_hudYOffset.load(std::memory_order_relaxed));
        }
        else if ((++b.copyFailures % 300) == 1)
            XrLog("[HUDLAYER] copy failed (%llu so far)",
                static_cast<unsigned long long>(b.copyFailures));
        return ok;
    }

public:
    bool HudConsuming() const
    {
        return ready_.load(std::memory_order_acquire) && sessionRunning_ &&
            g_hudLayerEnabled.load(std::memory_order_relaxed) && !hud_.failed;
    }

private:
    bool CreateEyeSwapchains()
    {
        std::uint32_t viewCount{};
        XrResult result = xrEnumerateViewConfigurationViews_(instance_, systemId_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
        if (XR_FAILED(result) || viewCount < 2)
        {
            XrLog("stereo view configuration unavailable result=%d count=%u", result, viewCount);
            return false;
        }
        std::vector<XrViewConfigurationView> configurations(viewCount);
        for (auto& config : configurations)
            config = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        result = xrEnumerateViewConfigurationViews_(instance_, systemId_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configurations.data());
        if (XR_FAILED(result))
        {
            XrLog("xrEnumerateViewConfigurationViews(data) failed result=%d", result);
            return false;
        }
        // [FPSAB] The runtime's own per-eye render size: the pixel density a
        // native VR title (REFramework etc.) renders at.  Compare with the
        // [SPLITFOV] rect to see how far below headset density the game sits.
        for (std::uint32_t viewIndex = 0; viewIndex < viewCount && viewIndex < 2; ++viewIndex)
            XrLog("[VIEWCFG] eye=%u runtime recommended=%ux%u max=%ux%u",
                viewIndex, configurations[viewIndex].recommendedImageRectWidth,
                configurations[viewIndex].recommendedImageRectHeight,
                configurations[viewIndex].maxImageRectWidth,
                configurations[viewIndex].maxImageRectHeight);
        for (std::uint32_t viewIndex = 0; viewIndex < viewCount && viewIndex < 2; ++viewIndex)
        {
            viewMaxWidth_[viewIndex] = configurations[viewIndex].maxImageRectWidth;
            viewMaxHeight_[viewIndex] = configurations[viewIndex].maxImageRectHeight;
        }

        std::uint32_t formatCount{};
        result = xrEnumerateSwapchainFormats_(session_, 0, &formatCount, nullptr);
        if (XR_FAILED(result) || formatCount == 0)
        {
            XrLog("xrEnumerateSwapchainFormats(count) failed result=%d count=%u", result, formatCount);
            return false;
        }
        std::vector<std::int64_t> formats(formatCount);
        result = xrEnumerateSwapchainFormats_(session_, formatCount, &formatCount, formats.data());
        if (XR_FAILED(result))
        {
            XrLog("xrEnumerateSwapchainFormats(data) failed result=%d", result);
            return false;
        }
        offeredFormats_ = formats;
        const std::array<DXGI_FORMAT, 7> preferred = {
            desktopFormat_,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R10G10B10A2_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        };
        swapchainFormat_ = 0;
        for (DXGI_FORMAT candidate : preferred)
        {
            for (std::int64_t offered : formats)
            {
                if (offered == candidate)
                {
                    swapchainFormat_ = offered;
                    break;
                }
            }
            if (swapchainFormat_)
                break;
        }
        if (!swapchainFormat_)
        {
            XrLog("runtime offered no supported color swapchain format");
            return false;
        }
        if (swapchainFormat_ != static_cast<std::int64_t>(desktopFormat_))
            XrLog("[FORMATBRIDGE] game format=%u runtime format=%lld; shader conversion enabled",
                static_cast<unsigned>(desktopFormat_), static_cast<long long>(swapchainFormat_));

        for (std::size_t eyeIndex = 0; eyeIndex < eyes_.size(); ++eyeIndex)
        {
            auto& eye = eyes_[eyeIndex];
            eye.width = static_cast<std::int32_t>(desktopWidth_);
            eye.height = static_cast<std::int32_t>(desktopHeight_);
            if (eye.width <= 0 || eye.height <= 0)
            {
                XrLog("desktop-sized eye swapchain unavailable; backbuffer size is %ux%u",
                    desktopWidth_, desktopHeight_);
                return false;
            }
            XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            createInfo.format = swapchainFormat_;
            createInfo.sampleCount = 1;
            createInfo.width = eye.width;
            createInfo.height = eye.height;
            createInfo.faceCount = 1;
            createInfo.arraySize = 1;
            createInfo.mipCount = 1;
            result = xrCreateSwapchain_(session_, &createInfo, &eye.handle);
            if (XR_FAILED(result))
            {
                XrLog("xrCreateSwapchain eye=%zu failed result=%d", eyeIndex, result);
                return false;
            }
            std::uint32_t imageCount{};
            result = xrEnumerateSwapchainImages_(eye.handle, 0, &imageCount, nullptr);
            if (XR_FAILED(result) || imageCount == 0)
            {
                XrLog("xrEnumerateSwapchainImages(count) eye=%zu failed result=%d count=%u",
                    eyeIndex, result, imageCount);
                return false;
            }
            eye.images.resize(imageCount);
            for (auto& image : eye.images)
                image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            result = xrEnumerateSwapchainImages_(eye.handle, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
            if (XR_FAILED(result))
            {
                XrLog("xrEnumerateSwapchainImages(data) eye=%zu failed result=%d", eyeIndex, result);
                return false;
            }
            eye.renderTargets.resize(imageCount);
            for (std::uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
            {
                D3D11_TEXTURE2D_DESC textureDesc{};
                eye.images[imageIndex].texture->GetDesc(&textureDesc);
                const DXGI_FORMAT targetFormat = ConcreteRenderTargetFormat(textureDesc.Format,
                    static_cast<DXGI_FORMAT>(swapchainFormat_));
                D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
                targetDesc.Format = targetFormat;
                targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                targetDesc.Texture2D.MipSlice = 0;
                HRESULT viewResult = device_->CreateRenderTargetView(eye.images[imageIndex].texture,
                    &targetDesc, &eye.renderTargets[imageIndex]);
                if (FAILED(viewResult))
                    viewResult = device_->CreateRenderTargetView(eye.images[imageIndex].texture, nullptr,
                        &eye.renderTargets[imageIndex]);
                if (FAILED(viewResult))
                {
                    XrLog("CreateRenderTargetView eye=%zu image=%u textureFormat=%u targetFormat=%u "
                        "bind=0x%X misc=0x%X hr=0x%08X failed", eyeIndex, imageIndex,
                        static_cast<unsigned>(textureDesc.Format), static_cast<unsigned>(targetFormat),
                        textureDesc.BindFlags, textureDesc.MiscFlags, static_cast<unsigned>(viewResult));
                    return false;
                }
                if (imageIndex == 0)
                    XrLog("eye %zu RTV typeless mapping %u -> %u bind=0x%X hr=0x%08X",
                        eyeIndex, static_cast<unsigned>(textureDesc.Format),
                        static_cast<unsigned>(targetFormat), textureDesc.BindFlags,
                        static_cast<unsigned>(viewResult));
            }
            XrLog("eye %zu swapchain ready size=%dx%d images=%u", eyeIndex,
                eye.width, eye.height, imageCount);
        }
        views_.resize(2);
        for (auto& view : views_)
            view = {XR_TYPE_VIEW};
        return true;
    }

    bool CompileShader(const char* source, const char* entry, const char* target, ID3DBlob** blob)
    {
        ID3DBlob* errors{};
        const HRESULT result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
        if (FAILED(result))
        {
            XrLog("D3DCompile %s/%s failed: %s", entry, target,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        }
        SafeRelease(errors);
        return SUCCEEDED(result);
    }


    // [SHARPEN] Contrast-adaptive sharpening of each eye image on its way into
    // the headset swapchain (replaces the plain copy when strength > 0).  The
    // neighbourhood min/max limits the gain where the image already has
    // contrast, so edges get crisper without halos.  Runs in a Reinhard-encoded
    // domain so HDR values above 1 are handled; alpha passes through.
    ID3D11PixelShader* sharpenShader_{};
    ID3D11VertexShader* sharpenVertexShader_{};   // own full-screen triangle (CreateD3DResources never runs)
    ID3D11Buffer* sharpenConstants_{};
    bool sharpenFailed_{};
    struct SharpenSource { ID3D11Texture2D* texture{}; ID3D11ShaderResourceView* view{}; };
    std::array<SharpenSource, 4> sharpenSources_{};
    std::uint64_t sharpenFrames_{}, sharpenSkips_{}, formatTransferFailures_{};

    bool EnsureSharpen()
    {
        if (sharpenShader_ && sharpenVertexShader_ && sharpenConstants_) return true;
        if (sharpenFailed_ || !device_) return false;
        static constexpr char vertexSource[] = R"(
            float4 VSFull(uint id : SV_VertexID) : SV_Position {
                const float2 uv = float2((id << 1) & 2, id & 2);
                return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
            }
        )";
        ID3DBlob* vertexBlob{};
        if (!CompileShader(vertexSource, "VSFull", "vs_5_0", &vertexBlob) ||
            FAILED(device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &sharpenVertexShader_)))
        {
            SafeRelease(vertexBlob);
            XrLog("[SHARPEN] vertex shader failed; plain copy kept");
            sharpenFailed_ = true;
            return false;
        }
        SafeRelease(vertexBlob);
        static constexpr char shader[] = R"(
            cbuffer Sharpen : register(b0) { float4 strength; };
            Texture2D source : register(t0);
            float3 Enc(float3 c) { c = max(c, 0.0); return c / (1.0 + c); }
            float3 Dec(float3 c) { c = min(c, 0.9999); return c / (1.0 - c); }
            float3 Tap(int2 p, int2 limit) { return Enc(source.Load(int3(clamp(p, int2(0, 0), limit), 0)).rgb); }
            float4 PSSharpen(float4 position : SV_Position) : SV_Target {
                uint w, h; source.GetDimensions(w, h);
                const int2 limit = int2(int(w) - 1, int(h) - 1);
                const int2 p = int2(position.xy);
                const float4 centre = source.Load(int3(p, 0));
                const float3 e = Enc(centre.rgb);
                const float3 a = Tap(p + int2(-1, -1), limit), b = Tap(p + int2(0, -1), limit), c = Tap(p + int2(1, -1), limit);
                const float3 d = Tap(p + int2(-1, 0), limit),                                   f = Tap(p + int2(1, 0), limit);
                const float3 g = Tap(p + int2(-1, 1), limit),  h2 = Tap(p + int2(0, 1), limit), i = Tap(p + int2(1, 1), limit);
                // Unsharp with a local-contrast limiter: push the pixel away
                // from the 3x3 average by k, allow it past the darkest /
                // brightest neighbour by at most half the local contrast times
                // strength (recovers softened detail), and cap the decoded
                // value at 1.25x the brightest neighbour so bright spots never
                // bloom into halos.  Flat areas are untouched.  k = 0 .. 3.
                const float s = saturate(strength.x);
                const float3 average = (a + c + g + i + 2.0 * (b + d + f + h2) + 4.0 * e) / 16.0;
                const float3 lo = min(min(min(min(a, b), min(c, d)), min(min(e, f), min(g, h2))), i);
                const float3 hi = max(max(max(max(a, b), max(c, d)), max(max(e, f), max(g, h2))), i);
                const float3 margin = 0.5 * s * (hi - lo);
                const float3 result = clamp(e + 3.0 * s * (e - average), lo - margin, hi + margin);
                const float3 outColour = min(Dec(saturate(result)), Dec(hi) * 1.25);
                return float4(max(outColour, 0.0), centre.a);
            }
        )";
        ID3DBlob* blob{};
        if (!CompileShader(shader, "PSSharpen", "ps_5_0", &blob))
        {
            XrLog("[SHARPEN] pixel shader failed; plain copy kept");
            sharpenFailed_ = true;
            return false;
        }
        HRESULT hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &sharpenShader_);
        SafeRelease(blob);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 16;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(hr))
            hr = device_->CreateBuffer(&desc, nullptr, &sharpenConstants_);
        if (FAILED(hr))
        {
            XrLog("[SHARPEN] resources failed hr=0x%08X; plain copy kept", static_cast<unsigned>(hr));
            SafeRelease(sharpenShader_);
            SafeRelease(sharpenConstants_);
            sharpenFailed_ = true;
            return false;
        }
        XrLog("[SHARPEN] contrast-adaptive sharpening ready");
        return true;
    }

    ID3D11ShaderResourceView* SharpenView(ID3D11Texture2D* texture)
    {
        for (auto& s : sharpenSources_)
            if (s.texture == texture && s.view) return s.view;
        for (auto& s : sharpenSources_)
        {
            if (s.texture) continue;
            if (FAILED(device_->CreateShaderResourceView(texture, nullptr, &s.view)) || !s.view)
                return nullptr;
            s.texture = texture;
            return s.view;
        }
        return nullptr;
    }

    void ReleaseSharpen()
    {
        for (auto& s : sharpenSources_) { SafeRelease(s.view); s.texture = nullptr; }
        SafeRelease(sharpenShader_);
        SafeRelease(sharpenVertexShader_);
        SafeRelease(sharpenConstants_);
    }

    // True when the eye image was written by the sharpening pass; false = the
    // caller does its plain copy.
    bool SharpenInto(ID3D11Texture2D* source, EyeSwapchain& eye, std::uint32_t image)
    {
        const float strength = g_sharpenStrength.load(std::memory_order_relaxed);
        if (!source || image >= eye.renderTargets.size() || !eye.renderTargets[image])
            return false;
        D3D11_TEXTURE2D_DESC sd{}, dd{};
        source->GetDesc(&sd);
        eye.images[image].texture->GetDesc(&dd);
        if (sd.Width != dd.Width || sd.Height != dd.Height)
        {
            if ((++sharpenSkips_ % 600) == 1)
                XrLog("[SHARPEN] skipped: source %ux%u vs eye image %ux%u", sd.Width, sd.Height, dd.Width, dd.Height);
            return false;
        }
        const auto destinationFormat = ConcreteRenderTargetFormat(dd.Format,
            static_cast<DXGI_FORMAT>(swapchainFormat_));
        const bool convert = RuntimeCompatibility::RequiresShaderTransfer(sd.Format,
            destinationFormat);
        if (!(strength > 0.001f) && !convert)
            return false;
        if (!EnsureSharpen())
            return false;
        ID3D11ShaderResourceView* view = SharpenView(source);
        if (!view)
        {
            if ((++sharpenSkips_ % 600) == 1)
                XrLog("[SHARPEN] skipped: no shader view for source format %u", static_cast<unsigned>(sd.Format));
            return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(immediateContext_->Map(sharpenConstants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        const float constants[4]{strength, 0.0f, 0.0f, 0.0f};
        std::memcpy(mapped.pData, constants, sizeof(constants));
        immediateContext_->Unmap(sharpenConstants_, 0);
        D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(dd.Width), static_cast<float>(dd.Height), 0.0f, 1.0f};
        immediateContext_->IASetInputLayout(nullptr);
        immediateContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        immediateContext_->VSSetShader(sharpenVertexShader_, nullptr, 0);
        immediateContext_->PSSetShader(sharpenShader_, nullptr, 0);
        immediateContext_->PSSetConstantBuffers(0, 1, &sharpenConstants_);
        immediateContext_->PSSetShaderResources(0, 1, &view);
        immediateContext_->OMSetRenderTargets(1, &eye.renderTargets[image], nullptr);
        immediateContext_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        immediateContext_->RSSetState(nullptr);
        immediateContext_->RSSetViewports(1, &viewport);
        immediateContext_->Draw(3, 0);
        ID3D11ShaderResourceView* noView{};
        ID3D11RenderTargetView* noTarget{};
        immediateContext_->PSSetShaderResources(0, 1, &noView);
        immediateContext_->OMSetRenderTargets(1, &noTarget, nullptr);
        if ((++sharpenFrames_ % 1800) == 1)
            XrLog("[SHARPEN] eye pass active strength=%.2f size=%ux%u sourceFmt=%u targetFmt=%u%s",
                strength, dd.Width, dd.Height, static_cast<unsigned>(sd.Format),
                static_cast<unsigned>(destinationFormat), convert ? " FORMAT-CONVERT" : "");
        return true;
    }

    bool TransferIntoEye(ID3D11Texture2D* source, EyeSwapchain& eye, std::uint32_t image)
    {
        if (!source || image >= eye.images.size() || !eye.images[image].texture)
            return false;
        D3D11_TEXTURE2D_DESC sourceDesc{}, destinationDesc{};
        source->GetDesc(&sourceDesc);
        eye.images[image].texture->GetDesc(&destinationDesc);
        const auto destinationFormat = ConcreteRenderTargetFormat(destinationDesc.Format,
            static_cast<DXGI_FORMAT>(swapchainFormat_));
        const bool convert = RuntimeCompatibility::RequiresShaderTransfer(sourceDesc.Format,
            destinationFormat);
        if (SharpenInto(source, eye, image))
            return true;
        if (convert)
        {
            if ((++formatTransferFailures_ % 120) == 1)
                XrLog("[FORMATBRIDGE] shader transfer failed source=%u target=%u",
                    static_cast<unsigned>(sourceDesc.Format),
                    static_cast<unsigned>(destinationFormat));
            return false;
        }
        immediateContext_->CopyResource(eye.images[image].texture, source);
        return true;
    }

    bool CreateD3DResources()
    {
        static constexpr char shader[] = R"(
            cbuffer Crop : register(b0) { float4 uvTransform; };
            Texture2D sourceTexture : register(t0);
            SamplerState linearClamp : register(s0);
            struct VOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
            VOut VSMain(uint id : SV_VertexID) {
                VOut output;
                float2 uv = float2((id << 1) & 2, id & 2);
                output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
                output.uv = uv;
                return output;
            }
            float4 PSMain(VOut input) : SV_Target {
                float2 uv = input.uv * uvTransform.xy + uvTransform.zw;
                return sourceTexture.Sample(linearClamp, uv);
            }
        )";
        ID3DBlob* vertexBlob{};
        ID3DBlob* pixelBlob{};
        if (!CompileShader(shader, "VSMain", "vs_5_0", &vertexBlob) ||
            !CompileShader(shader, "PSMain", "ps_5_0", &pixelBlob))
        {
            SafeRelease(vertexBlob);
            SafeRelease(pixelBlob);
            return false;
        }
        const HRESULT vertexResult = device_->CreateVertexShader(vertexBlob->GetBufferPointer(),
            vertexBlob->GetBufferSize(), nullptr, &vertexShader_);
        const HRESULT pixelResult = device_->CreatePixelShader(pixelBlob->GetBufferPointer(),
            pixelBlob->GetBufferSize(), nullptr, &pixelShader_);
        SafeRelease(vertexBlob);
        SafeRelease(pixelBlob);
        if (FAILED(vertexResult) || FAILED(pixelResult))
        {
            XrLog("CreateVertexShader/CreatePixelShader failed vs=0x%08X ps=0x%08X",
                static_cast<unsigned>(vertexResult), static_cast<unsigned>(pixelResult));
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&samplerDesc, &sampler_)))
        {
            XrLog("CreateSamplerState failed");
            return false;
        }

        const UvConstants constants[2] = {
            {0.5f, 1.0f, 0.0f, 0.0f},
            {0.5f, 1.0f, 0.5f, 0.0f},
        };
        for (std::size_t eye = 0; eye < constantBuffers_.size(); ++eye)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(UvConstants);
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{};
            initial.pSysMem = &constants[eye];
            if (FAILED(device_->CreateBuffer(&desc, &initial, &constantBuffers_[eye])))
            {
                XrLog("CreateBuffer crop constants eye=%zu failed", eye);
                return false;
            }
        }
        XrLog("SBS crop shaders and resources created");
        return true;
    }

    void PollEvents()
    {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent_(instance_, &event) == XR_SUCCESS)
        {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
                sessionState_ = changed.state;
                XrLog("session state -> %d", sessionState_);
                if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_)
                {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    const XrResult result = xrBeginSession_(session_, &beginInfo);
                    if (XR_SUCCEEDED(result))
                    {
                        sessionRunning_ = true;
                        XrLog("session running");
                    }
                    else
                        XrLog("xrBeginSession failed result=%d", result);
                }
                else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_)
                {
                    xrEndSession_(session_);
                    sessionRunning_ = false;
                }
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    XrFovf RenderedFov() const
    {
        const float halfVertical = (kRenderedVerticalFovDegrees * kPi / 180.0f) * 0.5f;
        const float halfHorizontal = std::atan(std::tan(halfVertical) * kRenderedAspect);
        XrFovf fov{};
        fov.angleLeft = -halfHorizontal;
        fov.angleRight = halfHorizontal;
        fov.angleUp = halfVertical;
        fov.angleDown = -halfVertical;
        return fov;
    }

    static XrFovf PresentationFov(const XrFovf& runtimeFov, float imageAspect, float scale)
    {
        const float horizontalTangent = (std::tan(runtimeFov.angleRight) -
            std::tan(runtimeFov.angleLeft)) * 0.5f * scale;
        const float verticalTangent = imageAspect > 0.0f
            ? horizontalTangent / imageAspect : horizontalTangent;
        const float halfHorizontal = std::atan(horizontalTangent);
        const float halfVertical = std::atan(verticalTangent);
        XrFovf fov{};
        fov.angleLeft = -halfHorizontal;
        fov.angleRight = halfHorizontal;
        fov.angleUp = halfVertical;
        fov.angleDown = -halfVertical;
        return fov;
    }

    bool EnsureSourceTexture(ID3D11Texture2D* backBuffer)
    {
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1)
        {
            XrLog("unsupported multisampled backbuffer count=%u", desc.SampleDesc.Count);
            return false;
        }
        if (sourceTexture_ && sourceWidth_ == desc.Width && sourceHeight_ == desc.Height &&
            sourceFormat_ == desc.Format)
            return true;
        SafeRelease(sourceView_);
        SafeRelease(sourceTexture_);
        sourceWidth_ = desc.Width;
        sourceHeight_ = desc.Height;
        sourceFormat_ = desc.Format;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &sourceTexture_)))
        {
            XrLog("CreateTexture2D source copy failed format=%d size=%ux%u", desc.Format,
                desc.Width, desc.Height);
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        viewDesc.Format = ShaderViewFormat(desc.Format);
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(sourceTexture_, &viewDesc, &sourceView_)))
        {
            XrLog("CreateShaderResourceView failed format=%d", viewDesc.Format);
            return false;
        }
        XrLog("source texture ready format=%d size=%ux%u", desc.Format, desc.Width, desc.Height);
        return true;
    }

    static DXGI_FORMAT ShaderViewFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return format;
        }
    }

    static DXGI_FORMAT ConcreteRenderTargetFormat(DXGI_FORMAT textureFormat, DXGI_FORMAT requestedFormat)
    {
        switch (textureFormat)
        {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return requestedFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return requestedFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default: return textureFormat;
        }
    }

    // [AFW] Shared RGBA16F texture (game device -> XR device) for the
    // synthesized eye, NT handle + keyed mutex like the main bridge.
    bool EnsureAfwBridge()
    {
        if (afwReady_)
            return true;
        if (afwFailed_ || !gameDevice_ || !device_ || !desktopWidth_ || !desktopHeight_)
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desktopWidth_;
        desc.Height = desktopHeight_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = desktopFormat_;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        HRESULT result = gameDevice_->CreateTexture2D(&desc, nullptr, &afwSharedGame_);
        IDXGIResource1* resource{};
        if (SUCCEEDED(result) && afwSharedGame_ &&
            SUCCEEDED(afwSharedGame_->QueryInterface(__uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&afwMutexGame_))) &&
            SUCCEEDED(afwSharedGame_->QueryInterface(__uuidof(IDXGIResource1),
                reinterpret_cast<void**>(&resource))) && resource)
        {
            result = resource->CreateSharedHandle(nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &afwHandle_);
            resource->Release();
            ID3D11Device1* xrDevice1{};
            if (SUCCEEDED(result) && afwHandle_ &&
                SUCCEEDED(device_->QueryInterface(__uuidof(ID3D11Device1),
                    reinterpret_cast<void**>(&xrDevice1))) && xrDevice1)
            {
                result = xrDevice1->OpenSharedResource1(afwHandle_, __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&afwSharedXr_));
                xrDevice1->Release();
            }
        }
        if (FAILED(result) || !afwSharedXr_ ||
            FAILED(afwSharedXr_->QueryInterface(__uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&afwMutexXr_))) || !afwMutexXr_)
        {
            XrLog("[AFW] bridge creation failed hr=0x%08X", static_cast<unsigned>(result));
            afwFailed_ = true;
            return false;
        }
        afwReady_ = true;
        XrLog("[AFW] bridge ready %ux%u", desktopWidth_, desktopHeight_);
        return true;
    }

    bool EnsureFullEyeBridge()
    {
        if (fullEyeReady_)
            return true;
        if (fullEyeFailed_ || !gameDevice_ || !device_ || !desktopWidth_ || !desktopHeight_)
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desktopWidth_;
        desc.Height = desktopHeight_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = desktopFormat_;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        HRESULT result = gameDevice_->CreateTexture2D(&desc, nullptr, &fullEyeSharedGame_);
        IDXGIResource1* resource{};
        if (SUCCEEDED(result) && fullEyeSharedGame_ &&
            SUCCEEDED(fullEyeSharedGame_->QueryInterface(__uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&fullEyeMutexGame_))) &&
            SUCCEEDED(fullEyeSharedGame_->QueryInterface(__uuidof(IDXGIResource1),
                reinterpret_cast<void**>(&resource))) && resource)
        {
            result = resource->CreateSharedHandle(nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &fullEyeHandle_);
            resource->Release();
            ID3D11Device1* xrDevice1{};
            if (SUCCEEDED(result) && fullEyeHandle_ &&
                SUCCEEDED(device_->QueryInterface(__uuidof(ID3D11Device1),
                    reinterpret_cast<void**>(&xrDevice1))) && xrDevice1)
            {
                result = xrDevice1->OpenSharedResource1(fullEyeHandle_, __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&fullEyeSharedXr_));
                xrDevice1->Release();
            }
        }
        if (FAILED(result) || !fullEyeSharedXr_ ||
            FAILED(fullEyeSharedXr_->QueryInterface(__uuidof(IDXGIKeyedMutex),
                reinterpret_cast<void**>(&fullEyeMutexXr_))) || !fullEyeMutexXr_)
        {
            XrLog("[FULLEYE] bridge creation failed hr=0x%08X", static_cast<unsigned>(result));
            fullEyeFailed_ = true;
            return false;
        }
        fullEyeReady_ = true;
        XrLog("[FULLEYE] bridge ready %ux%u", desktopWidth_, desktopHeight_);
        return true;
    }

    bool RenderEyes(IDXGISwapChain* desktopSwapchain, bool aerEnabled,
        std::size_t aerEye, bool nativeStereo = false)
    {
        PresentTiming::Scope timingScope(PresentTiming::EyeTotal);
        afwSynthesizedThisFrame_ = false;
        aerPairStagedThisFrame_ = false;
        aerPairReleasedThisFrame_ = false;
        ID3D11Texture2D* backBuffer{};
        if (FAILED(desktopSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&backBuffer))) || !backBuffer)
            return false;
        D3D11_TEXTURE2D_DESC backBufferDesc{};
        backBuffer->GetDesc(&backBufferDesc);
        if (backBufferDesc.Width != desktopWidth_ || backBufferDesc.Height != desktopHeight_ ||
            backBufferDesc.Format != desktopFormat_)
        {
            XrLog("backbuffer changed after OpenXR init: now=%ux%u fmt=%u expected=%ux%u fmt=%u",
                backBufferDesc.Width, backBufferDesc.Height,
                static_cast<unsigned>(backBufferDesc.Format), desktopWidth_, desktopHeight_,
                static_cast<unsigned>(desktopFormat_));
            backBuffer->Release();
            return false;
        }
        if (!sharedReady_ || !sharedTextureGame_ || !sharedTextureXr_ ||
            !gameImmediateContext_ || !sharedMutexGame_ || !sharedMutexXr_)
        {
            backBuffer->Release();
            return false;
        }
        // AER renders eye 0 and eye 1 on adjacent engine frames. Keep eye 0
        // off the runtime until eye 1 exists, then release the complete pair
        // together. EYEHOLD guarantees both staged images share one game
        // camera and rendered head pose.
        const bool pairLatch = aerEnabled &&
            !g_afwEnabled.load(std::memory_order_acquire);
        if (pairLatch && aerEye == 0 && EnsureFullEyeBridge() &&
            PresentTiming::Measure(PresentTiming::KeyedWait,
                [&]{return fullEyeMutexGame_->AcquireSync(0, 16);}) == S_OK)
        {
            gameImmediateContext_->CopyResource(fullEyeSharedGame_, backBuffer);
            const bool released = SUCCEEDED(fullEyeMutexGame_->ReleaseSync(1));
            backBuffer->Release();
            if (released)
            {
                aerPairStageValid_ = true;
                aerPairStagedThisFrame_ = true;
                const auto staged = ++aerPairsStaged_;
                if (staged <= 3 || (staged % 900) == 0)
                    XrLog("[AERPAIR] eye 0 staged; waiting for held eye 1 (staged=%llu)",
                        static_cast<unsigned long long>(staged));
            }
            return false;
        }
        if (PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return sharedMutexGame_->AcquireSync(0, 16);}) != S_OK)
        {
            backBuffer->Release();
            return false;
        }
        nativeDrawPose_ = NativeDrawPose::CaptureBoundary();
        // AER's FIFO owns the image slot. Draw association owns only its pose.
        // Switching slot identity between these routes caused repeated eyes
        // whenever the sampled draw association dropped out.
        traceCapture_ = GpuPoseTrace::CaptureBoundary();
        nativeDrawPoseConsumed_ = true;
        if (singlePoseGuardEnabled_)
        {
            AcquireSRWLockShared(&g_headPose.lock);
            const bool exact = g_headPose.valid && nativeDrawPose_.serial &&
                nativeDrawPose_.epoch == g_headPoseEpoch && IsSyncCameraView(nativeDrawPose_.view);
            ReleaseSRWLockShared(&g_headPose.lock);
            AerProjectionHistory::Views retained{};
            if (singlePoseContinuity_.Retain(exact, aerProjectionHistory_.Pair(retained)))
            {
                // No XR image has been acquired or released. Rescue below
                // submits the previous pair with its original pose and FOV.
                sharedMutexGame_->ReleaseSync(0);
                backBuffer->Release();
                ++singlePoseHolds_;
                return false;
            }
        }
        bool fullPair=false;
        if(nativeStereo && !aerEnabled && FullEye::ready && EnsureFullEyeBridge() && PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return fullEyeMutexGame_->AcquireSync(0,16);})==S_OK){
            // [FULLRES] the picture's size is read with the copy it describes.
            const unsigned pairWidth=FullEye::readyW,pairHeight=FullEye::readyH;
            fullPair=pairWidth&&pairHeight&&FullEye::CopyPair(gameImmediateContext_,sharedTextureGame_,fullEyeSharedGame_,g_splitSwap.load());
            if(fullPair){fullEyePendingWidth_=pairWidth;fullEyePendingHeight_=pairHeight;}
            fullEyeMutexGame_->ReleaseSync(fullPair?1:0);
        }
        if(!fullPair)gameImmediateContext_->CopyResource(sharedTextureGame_, backBuffer);
        const bool aerPair = pairLatch && aerEye == 1 && aerPairStageValid_ &&
            fullEyeReady_ && fullEyeSharedXr_ && fullEyeMutexXr_;
        backBuffer->Release();
        // [AFW] Synthesize the other eye from this one while the mod still holds the
        // game-side bridge (it is the colour source), then hand it across.
        bool afwSynth = false;
        float afwDeltaT = 0.0f;
        bool afwCamValid = false;
        float afwEyeX = 0.0f, afwEyeY = 0.0f, afwEyeZ = 0.0f;
        if (aerEnabled && g_afwEnabled.load(std::memory_order_acquire) && EnsureAfwBridge())
        {
            ID3D11Texture2D* depthTwin{};
            float cam[32]{};
            bool camValid = false;
            if (GBufferCensus::GetPresentedFrame(&depthTwin, cam, &camValid) && depthTwin)
            {
                AfwSynth::Params p{};
                p.p00 = camValid && cam[0] > 0.01f ? cam[0] : g_gameFocalX.load(std::memory_order_relaxed);
                p.p22 = camValid ? cam[10] : -1.0000167f;
                p.p32 = camValid ? cam[14] : -0.2000033f;
                // The camera writer puts the RENDERED eye at sign*half along
                // view x (eye 0 gets -half, flipped by SwapEyes).  The eye being
                // synthesized is the other one, at -sign*half, so the
                // translation from rendered to synthesized is -2*sign*half.
                // Validated offline: delta = target - source, positive when the
                // target eye is to the right of the source.
                float sign = aerEye == 0 ? -1.0f : 1.0f;
                if (AerControl::GetSwapEyes())
                    sign = -sign;
                p.deltaT = -2.0f * sign * AerControl::GetHalfEyeMeters();
                if (g_afwFlip.load(std::memory_order_acquire))
                    p.deltaT = -p.deltaT;
                afwDeltaT = p.deltaT;
                // [AFWPAIR] The depth must belong to the eye whose colour is
                // being warped.  The engine alternates two G-buffer families in
                // AER, so log the captured camera's view-space eye position:
                // it has to alternate by exactly one eye separation, in step.
                afwCamValid = camValid;
                if (camValid)
                {
                    // Row-vector world-to-view: eye position = -(t . columns).
                    const float* V = cam + 16;
                    afwEyeX = -(V[12] * V[0] + V[13] * V[1] + V[14] * V[2]);
                    afwEyeY = -(V[12] * V[4] + V[13] * V[5] + V[14] * V[6]);
                    afwEyeZ = -(V[12] * V[8] + V[13] * V[9] + V[14] * V[10]);
                }
                ID3D11Texture2D* synth = AfwSynth::Synthesize(gameDevice_, gameImmediateContext_,
                    sharedTextureGame_, depthTwin, p);
                if (synth && PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return afwMutexGame_->AcquireSync(0, 16);}) == S_OK)
                {
                    gameImmediateContext_->CopyResource(afwSharedGame_, synth);
                    afwMutexGame_->ReleaseSync(1);
                    afwSynth = true;
                }
            }
            (afwSynth ? g_afwSynthesized : g_afwSkipped).fetch_add(1, std::memory_order_relaxed);
            const auto total = g_afwSynthesized.load(std::memory_order_relaxed) + g_afwSkipped.load(std::memory_order_relaxed);
            if (total <= 6 || (total % 600) == 0)
                XrLog("[AFW] synthesized=%llu skipped=%llu (skips = no depth capture this frame) "
                    "eye=%zu(pinned) flip=%d deltaT=%+.4f [AFWPAIR] cam=%d capturedEye=(%.4f, %.4f, %.4f)",
                    static_cast<unsigned long long>(g_afwSynthesized.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_afwSkipped.load(std::memory_order_relaxed)), aerEye,
                    g_afwFlip.load(std::memory_order_relaxed) ? 1 : 0,
                    afwDeltaT, afwCamValid ? 1 : 0, afwEyeX, afwEyeY, afwEyeZ);
        }
        if (FAILED(sharedMutexGame_->ReleaseSync(1)))
            return false;
        if (PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return sharedMutexXr_->AcquireSync(1, 16);}) != S_OK)
            return false;

        bool fullHeld=false;
        if(fullPair || aerPair){fullHeld=PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return fullEyeMutexXr_->AcquireSync(1,16);})==S_OK;
            if(!fullHeld){sharedMutexXr_->ReleaseSync(0);return false;}}
        std::array<std::uint32_t, 2> acquired{};
        std::array<bool, 2> held{};
        bool ok = true;
        for (std::size_t eyeIndex = 0; eyeIndex < eyes_.size(); ++eyeIndex)
        {
            if (aerEnabled && eyeIndex != aerEye && !afwSynth && !aerPair)
                continue;
            XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrResult result = xrAcquireSwapchainImage_(eyes_[eyeIndex].handle, &acquireInfo,
                &acquired[eyeIndex]);
            if (XR_FAILED(result))
            {
                ok = false;
                break;
            }
            held[eyeIndex] = true;
            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            result = PresentTiming::Measure(PresentTiming::SwapWait,[&]{return xrWaitSwapchainImage_(eyes_[eyeIndex].handle, &waitInfo);});
            if (XR_FAILED(result))
            {
                ok = false;
                break;
            }
        }

        if (ok)
        {
            for (std::size_t eyeIndex = 0; eyeIndex < eyes_.size(); ++eyeIndex)
            {
                if (!held[eyeIndex])
                    continue;
                if (aerEnabled && afwSynth && eyeIndex != aerEye)
                {
                    // [AFW] the synthesized eye comes over its own bridge.
                    if (PresentTiming::Measure(PresentTiming::KeyedWait,[&]{return afwMutexXr_->AcquireSync(1, 16);}) == S_OK)
                    {
                        if (!TransferIntoEye(afwSharedXr_, eyes_[eyeIndex], acquired[eyeIndex]))
                            ok = false;
                        afwMutexXr_->ReleaseSync(0);
                        ShadowEyeForMeta(eyeIndex, eyes_[eyeIndex].images[acquired[eyeIndex]].texture);
                    }
                    else
                        ok = false;
                    continue;
                }
                ID3D11Texture2D* eyeSource =
                    (fullPair && eyeIndex == 1) ? fullEyeSharedXr_ :
                    (aerPair && eyeIndex == 0) ? fullEyeSharedXr_ :
                    sharedTextureXr_;
                if (!TransferIntoEye(eyeSource, eyes_[eyeIndex], acquired[eyeIndex]))
                    ok = false;
                ShadowEyeForMeta(eyeIndex, eyes_[eyeIndex].images[acquired[eyeIndex]].texture);
            }
            immediateContext_->Flush();
        }
        for (std::size_t eyeIndex = 0; eyeIndex < eyes_.size(); ++eyeIndex)
        {
            if (!held[eyeIndex])
                continue;
            eyeSwapchainsTouched_ = true;
            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (XR_FAILED(xrReleaseSwapchainImage_(eyes_[eyeIndex].handle, &releaseInfo)))
                ok = false;
        }
        if(fullHeld)fullEyeMutexXr_->ReleaseSync(0);
        sharedMutexXr_->ReleaseSync(0);
        if(ok)fullEyeFrame_=fullPair;
        if(ok&&fullPair){fullEyeWidth_=fullEyePendingWidth_;fullEyeHeight_=fullEyePendingHeight_;}
        if (aerPair)
        {
            aerPairStageValid_ = false;
            if (ok)
            {
                aerPairReleasedThisFrame_ = true;
                const auto released = ++aerPairsReleased_;
                if (released <= 3 || (released % 900) == 0)
                    XrLog("[AERPAIR] held pair released together (released=%llu)",
                        static_cast<unsigned long long>(released));
            }
        }
        if (ok && aerEnabled)
        {
            aerImageValid_[aerEye] = true;
            if (afwSynth || aerPair)
            {
                aerImageValid_[1 - aerEye] = true;
                if (afwSynth)
                    afwSynthesizedThisFrame_ = true;
            }
        }
        return ok;
    }

    void RenderFrame(IDXGISwapChain* desktopSwapchain, bool nativeStereoEnabled,
        bool aerEnabled)
    {
        eyeSwapchainsTouched_ = false;
        nativeDrawPoseConsumed_ = false;
        unsigned freshEyes = 0;
        NativeDrawPose::Initialize(MatchDrawCamera,desktopWidth_,desktopHeight_,aerEnabled);
        NativeDrawPose::SetActive((nativeStereoEnabled || aerEnabled) &&
            (aerEnabled || !g_nativeImageHold.load()));
        nativeDrawPose_ = {};
        GpuPoseTrace::Initialize(gameDevice_, gameImmediateContext_, MatchGpuCamera);
        GpuPoseTrace::SetNativeActive(nativeStereoEnabled && !aerEnabled && !g_nativeImageHold.load());
        traceCapture_ = 0;
        const bool trackingEnabled = nativeStereoEnabled || aerEnabled;
        const int trackingMode = aerEnabled ? (g_monoSubmit.load() ? 3 : 2) : (nativeStereoEnabled ? 1 : 0);
        if (trackingMode != lastTrackingMode_)
        {
            InvalidateHeadPose(trackingEnabled);
            aerImageValid_.fill(false);
            aerPoseValid_.fill(false);
            aerMarkerValid_ = false;
            aerPoseRoutes_[0] = aerPoseRoutes_[1] = 0;
            aerRescuedFrames_ = aerBlackFrames_ = aerSubmitReports_ = 0;
            lastTrackingMode_ = trackingMode;
            XrLog("camera head tracking mode=%s; center %s",
                aerEnabled ? (trackingMode == 3 ? "Mono" : "AER") : (nativeStereoEnabled ? "native-SBS" : "off"),
                trackingEnabled ? "requested" : "cleared");
        }
        if (aerEnabled != lastAerEnabled_)
        {
            if (aerPairStageValid_ && fullEyeMutexXr_ &&
                fullEyeMutexXr_->AcquireSync(1, 0) == S_OK)
                fullEyeMutexXr_->ReleaseSync(0);
            aerPairStageValid_ = false;
            aerImageValid_.fill(false);
            aerPoseValid_.fill(false);
            aerLastSeq_ = 0;
            aerCaptures_ = 0;
            aerRepeats_ = 0;
            aerMaxSeqGap_ = 0;
            aerSameEyeTwice_ = 0;
            aerResyncs_ = 0;
            aerStallPresents_ = 0;
            aerPrevCaptureEye_ = -1;
            g_aerRenderEye.store(0, std::memory_order_release);
            lastAerEnabled_ = aerEnabled;
            XrLog("AER image retention %s; eye cadence reset to left",
                aerEnabled ? "enabled" : "disabled");
        }

        const bool nativeOnly = nativeStereoEnabled && !aerEnabled && localSpace_;
        if (!nativeOnly || g_resetHeadPoseCenter.load(std::memory_order_acquire))
            g_nativeImageHold.store(false, std::memory_order_release);
        const bool holdRequested = g_nativeImageHold.load(std::memory_order_acquire);
        AcquireSRWLockShared(&g_headPose.lock);
        const auto holdEpoch = g_headPoseEpoch;
        ReleaseSRWLockShared(&g_headPose.lock);
        const bool reuseHeldImage = imageHold_.Reuse(
            holdRequested, nativeOnly, localSpace_, holdEpoch);
        if (!holdRequested && imageHoldFrames_)
        {
            XrLog("[IMAGEHOLD] resumed live images after %llu held submissions",
                static_cast<unsigned long long>(imageHoldFrames_));
            imageHoldFrames_ = 0;
        }
        RenderedHeadPose renderedHeadPose{};
        const bool useRenderedHeadPose = trackingEnabled && localSpace_ &&
            SnapshotRenderedHeadPose(renderedHeadPose, nativeStereoEnabled && !aerEnabled);
        // [TAGALL] Stereo only: display-time tags in Mono shake and make
        // icons follow the head.
        const bool tagCurrent = nativeStereoEnabled && !aerEnabled &&
            g_nativeTagCurrent.load(std::memory_order_relaxed);



        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        const long long waitStart = QpcNow();
        XrResult result = PresentTiming::Measure(PresentTiming::XrWait,[&]{return xrWaitFrame_(session_, &waitInfo, &frameState);});
        g_lastXrWaitMicros.store(static_cast<long long>(
            static_cast<double>(QpcNow() - waitStart) * 1e6 / static_cast<double>(QpcFrequency())),
            std::memory_order_relaxed);
        if (XR_FAILED(result))
            return;
        displayPeriodNs_ = frameState.predictedDisplayPeriod;
        PresentTiming::Period(frameState.predictedDisplayPeriod);
        // [LOOKAHEAD] which display time the pose handed to the game should
        // be predicted for: this present's, plus the measured depth.
        float lookahead = g_headLookaheadFrames.load(std::memory_order_relaxed);
        if (lookahead < 0.0f)
            lookahead = static_cast<float>(g_headPipelineDepth.load(std::memory_order_relaxed));
        lookahead = std::clamp(lookahead, 0.0f, 4.0f);
        const bool useLookahead = nativeStereoEnabled && !aerEnabled;
        const XrTime poseTime = frameState.predictedDisplayTime +
            (useLookahead && frameState.predictedDisplayPeriod > 0
                ? static_cast<XrTime>(lookahead *
                    static_cast<double>(frameState.predictedDisplayPeriod))
                : 0);
        NoteCadence(useLookahead ? lookahead : 0.0f);
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        result = PresentTiming::Measure(PresentTiming::XrBegin,[&]{return xrBeginFrame_(session_, &beginInfo);});
        if (XR_FAILED(result))
            return;
        // Stage all game-side copies before switching to the XR device.
        // Drain the batch in this frame, before building/submitting its layers.
        bool hudReady=false, hudWorldReady=false, targetReady=false, interactionReady=false, menuReady=false;
        bool objectiveReady[ObjectiveWorld::MaxMarkers]{};
        std::array<HudTransferRequest,ObjectiveWorld::MaxMarkers+5> hudRequests{};
        const bool worldEnabled=frameState.shouldRender && g_hudLensExact.load(std::memory_order_relaxed);
        hudRequests[0]={&hud_,&g_hudSource,&g_hudHasContent,!!frameState.shouldRender,&hudReady};
        hudRequests[1]={&hudWorld_,&g_hudWorldSource,&g_hudWorldHasContent,worldEnabled,&hudWorldReady};
        for(unsigned i=0;i<ObjectiveWorld::MaxMarkers;++i)
            hudRequests[i+2]={&objectives_[i],&g_objectiveSources[i],&g_objectiveHasContents[i],worldEnabled,&objectiveReady[i]};
        hudRequests[ObjectiveWorld::MaxMarkers+2]={&target_,&g_targetSource,&g_targetHasContent,!!frameState.shouldRender,&targetReady};
        hudRequests[ObjectiveWorld::MaxMarkers+3]={&interaction_,&g_interactionSource,&g_interactionHasContent,!!frameState.shouldRender,&interactionReady};
        hudRequests[ObjectiveWorld::MaxMarkers+4]={&menu_,&g_menuSource,&g_menuVisible,!!frameState.shouldRender,&menuReady};
        PrepareHudBatch(hudRequests);
        const auto interactionFrame=g_interactionFrame;
        const auto targetFrame=g_targetFrame;

        RenderedHeadPose objectiveCamera{};
        AcquireSRWLockShared(&g_headPose.lock);
        bool objectiveCameraValid=nativeStereoEnabled && !aerEnabled &&
            g_headFrameLatch.Snapshot(g_headPoseEpoch,objectiveCamera) &&
            IsSyncCameraView(objectiveCamera.markerView);
        ReleaseSRWLockShared(&g_headPose.lock);

        bool submit = false;
        // [AERSEQ] Which eye do the pixels in the backbuffer actually belong
        // to?  Ask the build queue, not a counter.
        // [MONO2D] A flat screen has nothing to build disparity from, so
        // capture the one frame into both eyes and leave the alternation
        // alone until a 3D camera comes back.
        const bool flatScene = aerEnabled &&
            g_flatSceneMono.load(std::memory_order_relaxed) &&
            g_sceneIsFlat.load(std::memory_order_relaxed);
        const bool stampsArmed = aerEnabled && !flatScene &&
            g_aerStampArmed.load(std::memory_order_acquire);
        std::uint64_t stampSeq = 0;
        int stampEye = 0;
        bool stampResynced = false;
        bool haveStamp = false;
        std::uint64_t stampHeadSerial = 0;
        alignas(16) float stampView[16]{};
        bool stampViewValid = false;
        if (stampsArmed)
            haveStamp = ConsumeAerStamp(aerLastSeq_, &stampSeq, &stampEye,
                &stampResynced, &stampHeadSerial, stampView, &stampViewValid);
        std::size_t renderedAerEye = haveStamp
            ? static_cast<std::size_t>(stampEye & 1)
            : static_cast<std::size_t>(
                g_aerRenderEye.load(std::memory_order_acquire) & 1);
        // [AFWPIN] With AFW on the engine renders ONE eye every frame and the
        // other is synthesized, so the stamp parity no longer describes the
        // pixels.  Alternating meant each eye's swapchain switched between a
        // real render and a reconstruction on every frame, and any residual
        // error in the reconstruction then read as a frame-rate left/right
        // shake rather than as a static offset.
        if (aerEnabled && g_afwEnabled.load(std::memory_order_acquire))
            renderedAerEye = 0;
        if (frameState.shouldRender)
        {
            XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = poseTime;
            locateInfo.space = trackingEnabled && localSpace_ ? localSpace_ : viewSpace_;
            XrViewState viewState{XR_TYPE_VIEW_STATE};
            std::uint32_t count{};
            result = xrLocateViews_(session_, &locateInfo, &viewState,
                static_cast<std::uint32_t>(views_.size()), &count, views_.data());
            const bool viewsLocated = XR_SUCCEEDED(result) && count >= 2 &&
                (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
            // [TAGMODE] the pose for this present's own display time, used as
            // the submit tag when tagging with the current pose.  Same as
            // views_ whenever no look-ahead is in effect.
            tagViewsValid_ = false;
            if (viewsLocated && tagCurrent)
            {
                if (poseTime == frameState.predictedDisplayTime)
                {
                    tagViews_ = views_;
                    tagViewsValid_ = true;
                }
                else
                {
                    XrViewLocateInfo tagLocate = locateInfo;
                    tagLocate.displayTime = frameState.predictedDisplayTime;
                    XrViewState tagState{XR_TYPE_VIEW_STATE};
                    std::uint32_t tagCount{};
                    if (tagViews_.size() != views_.size())
                        tagViews_.assign(views_.size(), XrView{XR_TYPE_VIEW});
                    for (auto& view : tagViews_)
                        view = {XR_TYPE_VIEW};
                    tagViewsValid_ = XR_SUCCEEDED(xrLocateViews_(session_, &tagLocate, &tagState,
                        static_cast<std::uint32_t>(tagViews_.size()), &tagCount, tagViews_.data())) &&
                        tagCount >= 2 && (tagState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
                }
            }
            bool captured = false;
            singlePoseGuardEnabled_ = false;
            if (viewsLocated)
            {
                runtimeEyeFov_[0] = views_[0].fov;   // [STEAMVRFOV]
                runtimeEyeFov_[1] = views_[1].fov;
                runtimeEyeFovValid_ = true;
                if (trackingEnabled && localSpace_)
                    PublishHeadPose(views_, viewState.viewStateFlags, nativeStereoEnabled && !aerEnabled);
                if (aerEnabled)
                {
                    AcquireSRWLockShared(&g_headPose.lock);
                    const auto epoch = g_headPoseEpoch;
                    ReleaseSRWLockShared(&g_headPose.lock);
                    const int mode = trackingMode * 2 + (flatScene ? 1 : 0);
                    const auto space = trackingEnabled && localSpace_ ? localSpace_ : viewSpace_;
                    // PublishHeadPose may recenter and change the epoch.
                    aerProjectionHistory_.Begin(mode, space, epoch);
                    singlePoseContinuity_.Begin(mode, space, epoch);
                    singlePoseGuardEnabled_ = trackingEnabled && localSpace_ && !flatScene;
                }
                if (reuseHeldImage)
                {
                    // Do not acquire/release/copy either eye swapchain. Both
                    // last-released images remain the same as the captured pair.
                    submit = true;
                }
                else if (flatScene)
                {
                    // Both eyes get this same frame at their own poses, which
                    // puts the flat image at infinity - the standard, and
                    // comfortable, way to show a 2D screen in a headset.
                    captured = RenderEyes(desktopSwapchain, false, 0, nativeStereoEnabled);
                    if (captured)
                    {
                        freshEyes = 3;
                        for (std::size_t eyeIndex = 0; eyeIndex < 2; ++eyeIndex)
                        {
                            aerRenderedHeadPoses_[eyeIndex].poses[0] =
                                useRenderedHeadPose ? renderedHeadPose.poses[0]
                                                    : HeadPoseMath::UprightPose(
                                                          views_[0].pose,
                                                          views_[0].pose.orientation);
                            aerRenderedHeadPoses_[eyeIndex].poses[1] =
                                useRenderedHeadPose ? renderedHeadPose.poses[1]
                                                    : HeadPoseMath::UprightPose(
                                                          views_[1].pose,
                                                          views_[0].pose.orientation);
                            aerRenderedHeadPoses_[eyeIndex].serial = 0;
                            aerPoseValid_[eyeIndex] = true;
                            aerImageValid_[eyeIndex] = true;
                        }
                        // Drop any builds queued before the flat screen; they
                        // describe a world that is no longer on screen.
                        aerLastSeq_ = g_aerStampSeq.load(std::memory_order_acquire);
                        aerPrevCaptureEye_ = -1;
                        aerStallPresents_ = 0;
                    }
                    submit = captured;
                }
                else if (stampsArmed && !haveStamp && aerStallPresents_ < 90)
                {
                    // The game has produced no new eye since the last present,
                    // so the backbuffer still holds the frame already
                    // consumed.  Copying it again would file one eye's pixels
                    // under the other eye's label - the mislabel this whole
                    // handshake exists to prevent.  Re-submit the retained
                    // pair untouched; the runtime keeps presenting each
                    // swapchain's last released image, so a present with no
                    // fresh render needs no copies at all.
                    ++aerRepeats_;
                    ++aerStallPresents_;
                    submit = aerImageValid_[0] && aerImageValid_[1] &&
                        aerPoseValid_[0] && aerPoseValid_[1];
                }
                else
                {
                    // Safety valve: if the camera build stops publishing
                    // altogether the retained pair would sit frozen in the
                    // headset forever.  After about a second and a half, go
                    // back to capturing on the alternating cadence so the view
                    // stays live, and say so - a stall this long is a bug
                    // somewhere upstream, not a quiet mode.
                    if (stampsArmed && !haveStamp)
                    {
                        if (aerStallPresents_ == 90)
                            XrLog("[AERSTALL] no camera build for %llu presents;"
                                " falling back to counted eye cadence",
                                static_cast<unsigned long long>(aerStallPresents_));
                        ++aerStallPresents_;
                    }
                    else
                        aerStallPresents_ = 0;
                const bool monoSubmit = aerEnabled && g_monoSubmit.load(std::memory_order_acquire);
                captured = RenderEyes(desktopSwapchain, aerEnabled && !monoSubmit, renderedAerEye,
                    nativeStereoEnabled);
                if (!captured && aerPairStagedThisFrame_)
                {
                    // The build queue must advance even though eye 0 is only
                    // staged. Until eye 1 arrives, keep submitting the last
                    // complete pair with its original poses.
                    if (haveStamp)
                    {
                        ++aerCaptures_;
                        if (stampResynced)
                        {
                            ++aerResyncs_;
                            const auto gap = stampSeq - aerLastSeq_;
                            if (gap > aerMaxSeqGap_)
                                aerMaxSeqGap_ = gap;
                        }
                        if (static_cast<int>(renderedAerEye) == aerPrevCaptureEye_)
                            ++aerSameEyeTwice_;
                        aerPrevCaptureEye_ = static_cast<int>(renderedAerEye);
                        aerLastSeq_ = stampSeq;
                    }
                    submit = aerImageValid_[0] && aerImageValid_[1] &&
                        aerPoseValid_[0] && aerPoseValid_[1];
                }
                else if (captured && aerEnabled)
                {
                    freshEyes = (monoSubmit || afwSynthesizedThisFrame_ ||
                        aerPairReleasedThisFrame_)
                        ? 3u : (1u << renderedAerEye);
                    // [AERTAG] AER used to tag every captured eye
                    // with the LATEST head sample at present time.  The game
                    // rendered it 1-2 samples earlier, and with [EYEHOLD] the
                    // second eye of a pair one sample earlier again, so each
                    // eye's reprojection error differed and changed every
                    // frame: a shake on every head turn.
                    // The stamp now names the sample the camera write
                    // applied; tag with exactly that one.
                    RenderedHeadPose stampPose{};
                    bool stampExact = false;
                    std::uint64_t latestSerial = 0, epochNow = 0;
                    // [RENDERCAM] the head sample matched from the camera the
                    // GPU actually rendered this frame with (stereo's route).
                    RenderedHeadPose renderedPose{};
                    bool renderedExact = false;
                    if (trackingEnabled && localSpace_)
                    {
                        AcquireSRWLockShared(&g_headPose.lock);
                        if (haveStamp && stampHeadSerial)
                            stampExact = g_headPose.valid &&
                                g_headCameraHistory.Sample(stampHeadSerial, stampPose);
                        // Same upload/draw/execute association as native stereo.
                        // Never replace a valid draw pose with the CPU FIFO.
                        renderedExact = g_headPose.valid && nativeDrawPose_.serial &&
                            nativeDrawPose_.epoch == g_headPoseEpoch &&
                            IsSyncCameraView(nativeDrawPose_.view);
                        if (renderedExact)
                        {
                            renderedPose.serial = nativeDrawPose_.serial;
                            renderedPose.poses[0] = nativeDrawPose_.eyes[0];
                            renderedPose.poses[1] = nativeDrawPose_.eyes[1];
                            renderedPose.renderedEye = nativeDrawPose_.renderedEye;
                            std::memcpy(renderedPose.markerView, nativeDrawPose_.view,
                                sizeof(renderedPose.markerView));
                        }
                        else
                            renderedExact = g_headPose.valid &&
                                g_headFrameLatch.Snapshot(g_headPoseEpoch, renderedPose) &&
                                IsSyncCameraView(renderedPose.markerView);
                        const bool drawSelected = nativeDrawPose_.serial &&
                            nativeDrawPose_.epoch == g_headPoseEpoch &&
                            IsSyncCameraView(nativeDrawPose_.view);
                        singlePoseContinuity_.Copied(freshEyes, drawSelected && g_headPose.valid);
                        ++aerPoseRoutes_[drawSelected ? 0 : 1];
                        latestSerial = g_headPose.serial;
                        epochNow = g_headPoseEpoch;
                        ReleaseSRWLockShared(&g_headPose.lock);
                    }
                    const auto poseRouteCount = aerPoseRoutes_[0] + aerPoseRoutes_[1];
                    if (poseRouteCount && (poseRouteCount <= 3 || (poseRouteCount % 600) == 0))
                        XrLog("[SINGLE_DRAWPOSE] mode=%s draw/fallback=%llu/%llu serial=%llu eye=%zu poseHolds=%llu",
                            monoSubmit ? "Mono" : "AER",
                            static_cast<unsigned long long>(aerPoseRoutes_[0]),
                            static_cast<unsigned long long>(aerPoseRoutes_[1]),
                            static_cast<unsigned long long>(nativeDrawPose_.serial), renderedAerEye,
                            static_cast<unsigned long long>(singlePoseHolds_));
                    if (renderedExact)
                    {
                        if (stampExact)
                            ++aerPair_[renderedPose.serial == stampHeadSerial ? 0
                                : (renderedPose.serial < stampHeadSerial ? 1 : 2)];
                        else
                            ++aerPair_[3];
                        aerRenderedHeadPoses_[renderedAerEye] = renderedPose;
                        ++aerTagRendered_;
                        if (renderedAerEye == 0 || monoSubmit)
                        {
                            std::memcpy(drawnView_, renderedPose.markerView, sizeof(drawnView_));
                            drawnValid_ = true;
                            if (drawnPrevSerial_ && renderedPose.serial >= drawnPrevSerial_)
                                ++drawnSteps_[std::min<std::uint64_t>(renderedPose.serial - drawnPrevSerial_, 3)];
                            drawnPrevSerial_ = renderedPose.serial;
                        }
                        ++aerTagAge_[latestSerial >= renderedPose.serial
                            ? std::min<std::uint64_t>(latestSerial - renderedPose.serial, 4) : 4];
                        // Icons: the rendered camera plus the head point it
                        // was rendered from (that eye in AER, the centre in Mono).
                        if (renderedAerEye == 0 || monoSubmit)
                        {
                            aerMarker_ = renderedPose;
                            if (monoSubmit)
                            {
                                aerMarker_.poses[0].position = {
                                    0.5f * (renderedPose.poses[0].position.x + renderedPose.poses[1].position.x),
                                    0.5f * (renderedPose.poses[0].position.y + renderedPose.poses[1].position.y),
                                    0.5f * (renderedPose.poses[0].position.z + renderedPose.poses[1].position.z)};
                            }
                            aerMarkerEpoch_ = epochNow;
                            aerMarkerValid_ = true;
                            ++aerMarkerOk_;
                        }
                    }
                    else if (stampExact)
                    {
                        aerRenderedHeadPoses_[renderedAerEye] = stampPose;
                        ++aerTagExact_;
                        ++aerTagAge_[latestSerial >= stampHeadSerial
                            ? std::min<std::uint64_t>(latestSerial - stampHeadSerial, 4) : 4];
                        // [AERMARKER] the same build is AER's world-icon camera -
                        // from eye 0's builds only.  Taking every build switched
                        // the icon camera between the two eye positions each
                        // frame; icons past the 20 m composition range are
                        // pulled in along the eye's ray, so they jumped by most
                        // of the eye separation every frame (the
                        // shake).  Stereo uses its left camera the same way.
                        if ((renderedAerEye == 0 || monoSubmit) && stampViewValid && IsSyncCameraView(stampView))
                        {
                            aerMarker_ = stampPose;
                            aerMarker_.poses[0] = stampPose.poses[renderedAerEye];
                            if (monoSubmit)
                            {
                                // [MONOVR] the Mono camera has no eye offset: place icons
                                // from the head centre (was the left/right eye in turn,
                                // a 3 cm shake every frame).
                                aerMarker_.poses[0].position = {
                                    0.5f * (stampPose.poses[0].position.x + stampPose.poses[1].position.x),
                                    0.5f * (stampPose.poses[0].position.y + stampPose.poses[1].position.y),
                                    0.5f * (stampPose.poses[0].position.z + stampPose.poses[1].position.z)};
                            }
                            std::memcpy(aerMarker_.markerView, stampView, sizeof(aerMarker_.markerView));
                            aerMarkerEpoch_ = epochNow;
                            aerMarkerValid_ = true;
                            ++aerMarkerOk_;
                        }
                        else if (renderedAerEye == 0)
                            ++aerMarkerBad_;
                    }
                    else if (useRenderedHeadPose)
                    {
                        aerRenderedHeadPoses_[renderedAerEye] = renderedHeadPose;
                        ++aerTagLatest_;
                    }
                    else
                    {
                        // [AERROLL] Roll stripped: the game rendered upright.
                        aerRenderedHeadPoses_[renderedAerEye].poses[0] =
                            HeadPoseMath::UprightPose(views_[0].pose, views_[0].pose.orientation);
                        aerRenderedHeadPoses_[renderedAerEye].poses[1] =
                            HeadPoseMath::UprightPose(views_[1].pose, views_[0].pose.orientation);
                        aerRenderedHeadPoses_[renderedAerEye].serial = 0;
                        g_aerRawTagFallbacks.fetch_add(1, std::memory_order_relaxed);
                    }
                    aerPoseValid_[renderedAerEye] = true;
                    if (tagCurrent && tagViewsValid_)
                    {
                        // [TAGALL] Stereo's tag, taken at capture and kept for as
                        // long as this image is shown.
                        for (std::size_t e = 0; e < 2; ++e)
                            aerRenderedHeadPoses_[renderedAerEye].poses[e] =
                                NativeLocatedPose(tagViews_[e].pose, tagViews_[0].pose.orientation);
                        ++aerTagCurrent_;
                    }
                    if (monoSubmit)
                    {
                        // [MONOVR] the one frame is in both eyes; each eye's
                        // own position is already in the pose (poses[0]/[1]).
                        aerRenderedHeadPoses_[1 - renderedAerEye] = aerRenderedHeadPoses_[renderedAerEye];
                        aerPoseValid_[0] = aerPoseValid_[1] = true;
                        aerImageValid_[0] = aerImageValid_[1] = true;
                    }
                    // [AERROLL] Both submitted eyes must carry the SAME
                    // upright orientation; any roll here head-locks that eye's
                    // world while the other eye stays world-fixed, which is
                    // what "the right eye tilts" looked like.
                    {
                        const auto reported = ++aerTagReports_;
                        if (reported <= 3 || (reported % 900) == 0)
                        {
                            float yaw0 = 0.0f, pitch0 = 0.0f, roll0 = 0.0f;
                            float yaw1 = 0.0f, pitch1 = 0.0f, roll1 = 0.0f;
                            float rawYaw = 0.0f, rawPitch = 0.0f, rawRoll = 0.0f;
                            QuaternionYawPitchRoll(
                                aerRenderedHeadPoses_[0].poses[0].orientation, yaw0, pitch0, roll0);
                            QuaternionYawPitchRoll(
                                aerRenderedHeadPoses_[1].poses[1].orientation, yaw1, pitch1, roll1);
                            QuaternionYawPitchRoll(views_[0].pose.orientation, rawYaw, rawPitch, rawRoll);
                            XrLog("[AERROLL] submitted tag roll L/R=%+.4f/%+.4f rad (both must be ~0), "
                                "headset roll=%+.4f, yaw L/R=%+.4f/%+.4f, rawFallbacks=%llu",
                                roll0, roll1, rawRoll, yaw0, yaw1,
                                static_cast<unsigned long long>(
                                    g_aerRawTagFallbacks.load(std::memory_order_relaxed)));
                        }
                    }
                    if (afwSynthesizedThisFrame_ || aerPairReleasedThisFrame_)
                    {
                        // AFW or the completed AER latch produced both eyes for
                        // this held head sample. Tag both with that exact pose.
                        aerRenderedHeadPoses_[1 - renderedAerEye] = aerRenderedHeadPoses_[renderedAerEye];
                        aerPoseValid_[1 - renderedAerEye] = true;
                    }
                    if (haveStamp)
                    {
                        // Alternation health.  With the queue, a resync (fell a
                        // whole ring behind: load, menu, mode switch) is the
                        // only legitimate discontinuity; same eye twice in a
                        // row means the cadence stalled and is a real bug
                        // again.
                        ++aerCaptures_;
                        if (stampResynced)
                        {
                            ++aerResyncs_;
                            const auto gap = stampSeq - aerLastSeq_;
                            if (gap > aerMaxSeqGap_)
                                aerMaxSeqGap_ = gap;
                        }
                        if (static_cast<int>(renderedAerEye) == aerPrevCaptureEye_)
                            ++aerSameEyeTwice_;
                        aerPrevCaptureEye_ = static_cast<int>(renderedAerEye);
                        aerLastSeq_ = stampSeq;
                        if ((aerCaptures_ % 900) == 0)
                        {
                            XrLog("[AERSEQ] captures=%llu repeats=%llu "
                                "sameEyeTwice=%u resyncs=%u maxSeqGap=%llu",
                                static_cast<unsigned long long>(aerCaptures_),
                                static_cast<unsigned long long>(aerRepeats_),
                                aerSameEyeTwice_, aerResyncs_,
                                static_cast<unsigned long long>(aerMaxSeqGap_));
                            XrLog("[MONOTAG] drawn camera matched exact=%llu blend=%llu nearest=%llu miss=%llu",
                                static_cast<unsigned long long>(g_monoTagExact.load(std::memory_order_relaxed)),
                                static_cast<unsigned long long>(g_monoTagBlend.load(std::memory_order_relaxed)),
                                static_cast<unsigned long long>(g_monoTagNearest.load(std::memory_order_relaxed)),
                                static_cast<unsigned long long>(g_monoTagMiss.load(std::memory_order_relaxed)));
                            XrLog("[RENDERCAM] tags from rendered camera=%llu, build stamp=%llu | rendered vs stamp head sample: "
                                "same=%llu rendered-older=%llu rendered-newer=%llu no-stamp=%llu | camera match/miss=%llu/%llu",
                                static_cast<unsigned long long>(aerTagRendered_),
                                static_cast<unsigned long long>(aerTagExact_),
                                static_cast<unsigned long long>(aerPair_[0]),
                                static_cast<unsigned long long>(aerPair_[1]),
                                static_cast<unsigned long long>(aerPair_[2]),
                                static_cast<unsigned long long>(aerPair_[3]),
                                static_cast<unsigned long long>(g_headCameraMatch.load(std::memory_order_relaxed)),
                                static_cast<unsigned long long>(g_headCameraMiss.load(std::memory_order_relaxed)));
                            XrLog("[AERTAG] exact=%llu latest-fallback=%llu | render-pose age in samples "
                                "0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu | world-icon camera ok=%llu bad=%llu",
                                static_cast<unsigned long long>(aerTagExact_),
                                static_cast<unsigned long long>(aerTagLatest_),
                                static_cast<unsigned long long>(aerTagAge_[0]),
                                static_cast<unsigned long long>(aerTagAge_[1]),
                                static_cast<unsigned long long>(aerTagAge_[2]),
                                static_cast<unsigned long long>(aerTagAge_[3]),
                                static_cast<unsigned long long>(aerTagAge_[4]),
                                static_cast<unsigned long long>(aerMarkerOk_),
                                static_cast<unsigned long long>(aerMarkerBad_));
                        }
                    }
                    else
                        g_aerRenderEye.store(static_cast<int>(1 - renderedAerEye),
                            std::memory_order_release);
                    submit = aerImageValid_[0] && aerImageValid_[1] &&
                        aerPoseValid_[0] && aerPoseValid_[1];
                }
                else
                    submit = captured;
                }
            }
            const std::uint64_t attempt = ++frameAttempts_;
            if (attempt <= 4 || (!submit && (attempt % 300) == 0))
                XrLog("frame attempt=%llu mode=%s shouldRender=%u locate=%d count=%u "
                    "flags=0x%llX captured=%d submit=%d aerEye=%zu",
                    static_cast<unsigned long long>(attempt),
                    aerEnabled ? "AER" : (nativeStereoEnabled ? "SPLIT" : "MONO"),
                    frameState.shouldRender, result, count,
                    static_cast<unsigned long long>(viewState.viewStateFlags),
                    captured ? 1 : 0, submit ? 1 : 0, renderedAerEye);
        }
        else
        {
            const std::uint64_t attempt = ++frameAttempts_;
            if (attempt <= 4 || (attempt % 300) == 0)
                XrLog("frame attempt=%llu mode=%s suppressed: runtime shouldRender=0 "
                    "sessionState=%d", static_cast<unsigned long long>(attempt),
                    aerEnabled ? "AER" : (nativeStereoEnabled ? "SPLIT" : "MONO"),
                    sessionState_);
        }

        // A present advances the game image even if its XR copy/locate failed.
        // Consume the corresponding FIFO entry so the next image cannot inherit
        // a failed frame's eye. Discard unconsumed draw evidence at that boundary.
        if (aerEnabled)
        {
            if (haveStamp) aerLastSeq_ = stampSeq;
            if (!freshEyes && !nativeDrawPoseConsumed_) NativeDrawPose::CaptureBoundary();
        }
        AcquireSRWLockShared(&g_headPose.lock);
        const bool useDrawPose = nativeStereoEnabled && !aerEnabled && nativeDrawPose_.serial &&
            nativeDrawPose_.epoch == g_headPoseEpoch;
        ReleaseSRWLockShared(&g_headPose.lock);
        const bool videoMode = nativeStereoEnabled && !aerEnabled &&
            !g_splitMonoFallback.load(std::memory_order_relaxed) &&
            g_flatPresents.load(std::memory_order_relaxed) >= kVideoEnterPresents;
        {
            const auto layout = FullEye::lastLayout;
            if (videoMode != videoModeLogged_ ||
                (videoMode && (layout.kind != videoLayoutLogged_ || (++videoFrames_ % 600) == 0)))
            {
                XrLog("[VIDEOFIT] %s: flatPresents=%u fullEye=%d layout=%d rect=%.0fx%.0f+%.0f+%.0f screenFovH=%.0f",
                    videoMode ? "flat screen ON" : "flat screen off",
                    g_flatPresents.load(std::memory_order_relaxed), fullEyeFrame_ ? 1 : 0, layout.kind,
                    layout.w, layout.h, layout.x, layout.y, VideoScreenFovH());
                videoModeLogged_ = videoMode;
                videoLayoutLogged_ = layout.kind;
            }
        }
        // [DOCK] a flat screen (video, loading screen) pinned to the direction
        // you were facing when it appeared, level, instead of re-placed at the
        // head every frame.  Recenter re-docks it.
        const bool dockActive = (videoMode || flatScene) && trackingEnabled && localSpace_ &&
            g_dockFlatScreens.load(std::memory_order_relaxed);
        if (!dockActive)
            dockValid_ = false;
        else
        {
            AcquireSRWLockShared(&g_headPose.lock);
            const auto epoch = g_headPoseEpoch;
            ReleaseSRWLockShared(&g_headPose.lock);
            if (!dockValid_ || epoch != dockEpoch_)
            {
                const auto f = RotateByQuaternion(views_[0].pose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
                const float yaw = std::hypot(f.x, f.z) > 1e-4f ? std::atan2(-f.x, -f.z) : 0.0f;
                dockOrientation_ = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
                dockValid_ = true;
                dockEpoch_ = epoch;
                XrLog("[DOCK] flat screen docked, facing %.1f deg", yaw * 57.29578f);
            }
        }
        std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        if (aerEnabled)
        {
            AcquireSRWLockShared(&g_headPose.lock);
            const auto projectionEpoch = g_headPoseEpoch;
            ReleaseSRWLockShared(&g_headPose.lock);
            aerProjectionHistory_.Begin(trackingMode * 2 + (flatScene ? 1 : 0),
                trackingEnabled && localSpace_ ? localSpace_ : viewSpace_, projectionEpoch);
            // A failed transfer may have released an unfilled/new image. Old
            // metadata cannot be used to rescue that image as a retained pair.
            if (!freshEyes && eyeSwapchainsTouched_)
            {
                aerProjectionHistory_.Reset();
                singlePoseContinuity_.Reset();
                aerImageValid_.fill(false);
                aerPoseValid_.fill(false);
            }
        }
        if (submit)
        {
            for (std::size_t eyeIndex = 0; eyeIndex < projectionViews.size(); ++eyeIndex)
            {
                auto& view = projectionViews[eyeIndex];
                view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                const std::size_t sourceView = (nativeStereoEnabled || aerEnabled)
                    ? eyeIndex : 0;
                if (aerEnabled && aerPoseValid_[eyeIndex])
                    view.pose = aerRenderedHeadPoses_[eyeIndex].poses[sourceView];
                else if (useDrawPose)
                    view.pose = nativeDrawPose_.eyes[sourceView];
                else if (tagCurrent && tagViewsValid_)
                    view.pose = NativeLocatedPose(
                        tagViews_[sourceView].pose, tagViews_[0].pose.orientation);
                else if (useRenderedHeadPose)
                    view.pose = renderedHeadPose.poses[sourceView];
                else if (nativeStereoEnabled && !aerEnabled)
                    view.pose = NativeLocatedPose(
                        views_[sourceView].pose, views_[0].pose.orientation);
                else
                    view.pose = views_[sourceView].pose;

                if (eyeIndex == 0 && traceCapture_)
                {
                    AcquireSRWLockShared(&g_headPose.lock);
                    const auto latest = g_headPose.serial;
                    const auto epoch = g_headPoseEpoch;
                    ReleaseSRWLockShared(&g_headPose.lock);
                    GpuPoseTrace::PoseAssigned(traceCapture_, view.pose,
                        useRenderedHeadPose ? renderedHeadPose.serial : 0, latest, epoch,
                        useDrawPose ? nativeDrawPose_.serial : 0);
                }
                const std::int32_t halfWidth = eyes_[eyeIndex].width / 2;
                std::int32_t imageWidth = eyes_[eyeIndex].width;
                std::int32_t imageOffsetX = 0;
                std::int32_t imageHeight = eyes_[eyeIndex].height;
                std::int32_t imageOffsetY = 0;
                if (videoMode)
                {
                    // [VIDEOFIT] the region the game drew into; the whole
                    // image when the per-eye capture produced one.
                    const auto layout = FullEye::lastLayout;
                    if (!fullEyeFrame_ && layout.kind != 0)
                    {
                        const auto clampTo = [](float v, std::int32_t lo, std::int32_t hi) {
                            return std::clamp(static_cast<std::int32_t>(std::lround(v)), lo, hi);
                        };
                        imageOffsetX = clampTo(layout.x, 0, eyes_[eyeIndex].width - 16);
                        imageOffsetY = clampTo(layout.y, 0, eyes_[eyeIndex].height - 16);
                        imageWidth = clampTo(layout.w, 16, eyes_[eyeIndex].width - imageOffsetX);
                        imageHeight = clampTo(layout.h, 16, eyes_[eyeIndex].height - imageOffsetY);
                    }
                }
                else if (nativeStereoEnabled && fullEyeFrame_)
                {
                    // [FULLRES] the per-eye picture sits 1:1 in the top-left of
                    // the eye image: the whole image at 100 % render size, less
                    // below it.  Same picture, same lens claim.
                    if (fullEyeWidth_ >= 16 && fullEyeHeight_ >= 16)
                    {
                        imageWidth = std::min<std::int32_t>(imageWidth,
                            static_cast<std::int32_t>(fullEyeWidth_));
                        imageHeight = std::min<std::int32_t>(imageHeight,
                            static_cast<std::int32_t>(fullEyeHeight_));
                    }
                }
                else if (nativeStereoEnabled)
                {
                    // [AUTOMONO] one output this frame: the whole frame is one
                    // 16:9 mono picture; both eyes get it, tracking kept.
                    const bool splitMono = g_splitMonoFallback.load(std::memory_order_relaxed);
                    const std::int32_t rectWidth = splitMono ? eyes_[eyeIndex].width : halfWidth;
                    imageWidth = rectWidth;
                    const bool rightHalf = !splitMono && ((eyeIndex != 0) !=
                        g_splitSwap.load(std::memory_order_relaxed));
                    imageOffsetX = rightHalf ? halfWidth : 0;
                    // [SPLITRECT] Each native eye is a full-width 16:9 picture
                    // letterboxed in the middle of its 1920x2160 half (square
                    // pixels, black bars above and below).  Confirmed in
                    // the headset: horizontal from 1/P00 with vertical derived
                    // from the half's full height gave correct proportions
                    // and a black border, which only a centred 16:9 picture
                    // satisfies.  Submit just that picture: height =
                    // halfWidth * P00 / P11 (1080 for a 16:9 lens). Hinny that GOAT.
                    const float rectFocalX = g_gameFocalX.load(std::memory_order_relaxed);
                    const float rectFocalY = g_gameFocalY.load(std::memory_order_relaxed);
                    if (rectFocalX > 0.01f && rectFocalY > 0.01f)
                    {
                        const auto content = static_cast<std::int32_t>(std::lround(
                            static_cast<float>(rectWidth) * rectFocalX / rectFocalY));
                        if (content > 16 && content < eyes_[eyeIndex].height)
                        {
                            imageHeight = content;
                            imageOffsetY = (eyes_[eyeIndex].height - content) / 2;
                        }
                    }
                }
                else if (aerEnabled && eyes_[eyeIndex].width > eyes_[eyeIndex].height &&
                    g_gameFocalX.load(std::memory_order_relaxed) <= 0.01f)
                {
                    // Square crop only when the honest lens claim is NOT
                    // driving.  It exists to avoid a letterboxed presentation,
                    // but it throws away the game's horizontal field: a 16:9
                    // render is 76.7 degrees wide against 48 tall, so cropping
                    // to square discards nearly 30 degrees of real FOV.  With
                    // a truthful per-axis claim the full width can be shown
                    // instead, which fills far more of the headset without
                    // over-claiming anything.
                    // AER uses one full-height centered square per eye. This removes the
                    // vertically letterboxed 16:9-in-a-headset presentation without
                    // stretching the image; only peripheral desktop width is cropped.
                    imageWidth = eyes_[eyeIndex].height;
                    imageOffsetX = (eyes_[eyeIndex].width - imageWidth) / 2;
                }
                const float imageAspect = eyes_[eyeIndex].height > 0
                    ? static_cast<float>(imageWidth) /
                        static_cast<float>(eyes_[eyeIndex].height) : 1.0f;
                const float fovScale = nativeStereoEnabled ? kLaneBFovScale
                    : (aerEnabled ? g_fillFactor.load(std::memory_order_relaxed) : 1.0f);
                view.fov = PresentationFov(views_[sourceView].fov, imageAspect, fovScale);
                if (aerEnabled)
                {
                    const float focalX = g_gameFocalX.load(std::memory_order_relaxed);
                    const float focalY = g_gameFocalY.load(std::memory_order_relaxed);
                    const float referenceX =
                        g_gameReferenceFocalX.load(std::memory_order_relaxed);
                    const float referenceY =
                        g_gameReferenceFocalY.load(std::memory_order_relaxed);
                    // Claim the lens the game ACTUALLY rendered, not a ratio
                    // against the headset's own frustum.
                    //
                    // AER submits a centred square crop of full image height.
                    // For a perspective projection that crop's horizontal
                    // half-angle equals its vertical one, because
                    // tan(fovH/2) = aspect * tan(fovV/2) and the crop divides
                    // the width by exactly that aspect.  So the honest claim
                    // is fovV on BOTH axes, and tan(fovV/2) = 1 / focalY.
                    //
                    // Claiming roughly the headset's frustum for an image
                    // containing only ~48 degrees of world would be a 2x
                    // over-claim that magnifies the image and compresses
                    // disparity (usable depth would need a ~128 mm IPD).
                    if (focalY > 0.01f && referenceY > 0.01f)
                    {
                        // Direct, no smoothing: the fed value is already
                        // exactly the lens this frame rendered with, and any
                        // lag between claim and render IS the size-jump bug.
                        const float verticalHalf = std::atan(std::clamp(
                            fovScale / focalY, 0.05f, 6.0f));
                        // Horizontal comes from focalX when the full-width
                        // image is submitted; for a centred square crop the
                        // horizontal half-angle equals the vertical one.
                        const bool squareCrop =
                            imageWidth != eyes_[eyeIndex].width;
                        const float horizontalHalf = squareCrop
                            ? verticalHalf
                            : std::atan(std::clamp(fovScale / focalX, 0.05f,
                                6.0f));
                        view.fov.angleUp = verticalHalf;
                        view.fov.angleDown = -verticalHalf;
                        view.fov.angleLeft = -horizontalHalf;
                        view.fov.angleRight = horizontalHalf;
                    }
                    const float convergence = g_monoSubmit.load(std::memory_order_relaxed)
                        ? 0.0f : g_aerConvergence.load(std::memory_order_relaxed);   // [MONOVR]
                    const float shift = eyeIndex == 0 ? convergence : -convergence;
                    view.fov.angleLeft = std::atan(std::tan(view.fov.angleLeft) + shift);
                    view.fov.angleRight = std::atan(std::tan(view.fov.angleRight) + shift);
                }
                else if (nativeStereoEnabled)
                {
                    // [SPLIT] Claim exactly the lens the game rendered, for
                    // exactly the picture it rendered it into ([SPLITRECT]:
                    // the centred 16:9 region of the half).  The engine skew
                    // is zeroed at the source ([SKEWZERO]), so the claim is
                    // symmetric.  What goes wrong otherwise:
                    // 16:9 lens over the whole 2160-tall half crushed the
                    // picture into the middle ("squished vertically"); an 8:9
                    // horizontal shrank it further ("not filling").
                    const float focalX = g_gameFocalX.load(std::memory_order_relaxed);
                    const float focalY = g_gameFocalY.load(std::memory_order_relaxed);
                    const float splitFill = g_splitFill.load(std::memory_order_relaxed);
                    if (focalX > 0.01f && focalY > 0.01f)
                    {
                        const float verticalHalf =
                            std::atan(std::clamp(splitFill / focalY, 0.05f, 6.0f));
                        const float horizontalHalf =
                            std::atan(std::clamp(splitFill / focalX, 0.05f, 6.0f));
                        view.fov.angleUp = verticalHalf;
                        view.fov.angleDown = -verticalHalf;
                        view.fov.angleLeft = -horizontalHalf;
                        view.fov.angleRight = horizontalHalf;
                    }
                    const float convergence = g_splitConvergence.load(std::memory_order_relaxed);
                    // [MESWAP] keyed to the game's eye, not the headset eye:
                    // with Swap eyes on, the plane inverts.
                    const bool gameLeft = (eyeIndex == 0) != g_splitSwap.load(std::memory_order_relaxed);
                    const float shift = gameLeft ? convergence : -convergence;
                    view.fov.angleLeft = std::atan(std::tan(view.fov.angleLeft) + shift);
                    view.fov.angleRight = std::atan(std::tan(view.fov.angleRight) + shift);
                }
                if (videoMode)
                {
                    // [VIDEOFIT] flat screen: fixed width, the picture's own
                    // aspect, same image and fresh upright pose for both eyes.
                    const float aspect = imageHeight > 0
                        ? static_cast<float>(imageWidth) / static_cast<float>(imageHeight) : 16.0f / 9.0f;
                    const float horizontalHalf = VideoScreenFovH() * 0.5f * 3.14159265f / 180.0f;
                    const float verticalHalf = std::atan(std::tan(horizontalHalf) / std::max(aspect, 0.1f));
                    view.fov.angleLeft = -horizontalHalf;
                    view.fov.angleRight = horizontalHalf;
                    view.fov.angleUp = verticalHalf;
                    view.fov.angleDown = -verticalHalf;
                    view.pose = NativeLocatedPose(views_[sourceView].pose, views_[0].pose.orientation);
                }
                if (dockActive)
                    view.pose.orientation = dockOrientation_;   // [DOCK]
                view.subImage.swapchain = eyes_[eyeIndex].handle;
                view.subImage.imageRect.offset = {imageOffsetX, imageOffsetY};
                view.subImage.imageRect.extent = {imageWidth, imageHeight};
                view.subImage.imageArrayIndex = 0;
                if (aerEnabled)
                {
                    // Stale eye: keep its own pose, FOV and crop together.
                    // Only a successful fresh transfer replaces its metadata.
                    if ((freshEyes & (1u << eyeIndex)) ||
                        !aerProjectionHistory_.Load(eyeIndex, view))
                        aerProjectionHistory_.Store(eyeIndex, view);
                }
            }
            // [WORLDLOCK] the world as submitted = tag rotation * drawn camera.
            // With the stick idle it must stay still; its frame-to-frame
            // jitter is the shake, whatever causes it.
            {
                const int mode = aerEnabled ? (g_monoSubmit.load(std::memory_order_relaxed) ? 2 : 1)
                    : (nativeStereoEnabled ? 0 : -1);
                if (mode != worldMode_)
                {
                    worldShown_.Reset(); worldTag_.Reset(); worldHead_.Reset();
                    drawnValid_ = false; drawnPrevSerial_ = 0;
                    std::fill(std::begin(drawnSteps_), std::end(drawnSteps_), 0ull);
                    worldMode_ = mode;
                }
                if (mode == 0 && objectiveCameraValid)
                {
                    std::memcpy(drawnView_, objectiveCamera.markerView, sizeof(drawnView_));
                    drawnValid_ = true;
                    if (drawnPrevSerial_ && objectiveCamera.serial >= drawnPrevSerial_)
                        ++drawnSteps_[std::min<std::uint64_t>(objectiveCamera.serial - drawnPrevSerial_, 3)];
                    drawnPrevSerial_ = objectiveCamera.serial;
                }
                if (mode >= 0 && drawnValid_ && !videoMode && !flatScene && trackingEnabled && localSpace_)
                {
                    float head[9]{}, tag[9]{}, drawn[9]{}, shown[9]{};
                    const auto& h = views_[0].pose.orientation;
                    const auto& t = projectionViews[0].pose.orientation;
                    WorldLock::QuatRotation(h.x, h.y, h.z, h.w, head);
                    WorldLock::QuatRotation(t.x, t.y, t.z, t.w, tag);
                    WorldLock::ViewRotation(drawnView_, drawn);
                    WorldLock::Mul(tag, drawn, shown);
                    const bool moving = worldHead_.have >= 1 && WorldLock::AngleDeg(head, worldHead_.prev) > 0.1f;
                    worldHead_.Add(head, moving);
                    worldTag_.Add(tag, moving);
                    worldShown_.Add(shown, moving);
                    if (worldShown_.n >= 600)
                    {
                        static const char* names[] = {"Stereo", "AER", "Mono"};
                        XrLog("[WORLDLOCK] %s over %llu frames: WORLD SHOWN jitter rms %.3f deg (head moving %.3f, %llu frames) max %.3f, drift %.3f deg/frame"
                            " | tag jitter rms %.3f | headset jitter rms %.3f, turn %.3f deg/frame | drawn head sample steps 0/1/2/3+=%llu/%llu/%llu/%llu",
                            names[mode], static_cast<unsigned long long>(worldShown_.n),
                            worldShown_.Rms(), worldShown_.RmsMoving(), static_cast<unsigned long long>(worldShown_.moving),
                            worldShown_.jitterMax, worldShown_.MeanStep(),
                            worldTag_.Rms(), worldHead_.Rms(), worldHead_.MeanStep(),
                            static_cast<unsigned long long>(drawnSteps_[0]), static_cast<unsigned long long>(drawnSteps_[1]),
                            static_cast<unsigned long long>(drawnSteps_[2]), static_cast<unsigned long long>(drawnSteps_[3]));
                        worldShown_.n = worldShown_.moving = 0; worldShown_.step = worldShown_.jitter2 = worldShown_.jitterMax = worldShown_.jitterMoving2 = 0.0;
                        worldTag_.n = worldTag_.moving = 0; worldTag_.step = worldTag_.jitter2 = worldTag_.jitterMax = worldTag_.jitterMoving2 = 0.0;
                        worldHead_.n = worldHead_.moving = 0; worldHead_.step = worldHead_.jitter2 = worldHead_.jitterMax = worldHead_.jitterMoving2 = 0.0;
                        std::fill(std::begin(drawnSteps_), std::end(drawnSteps_), 0ull);
                    }
                }
            }
            if (reuseHeldImage)
                projectionViews = imageHold_.Projection();
            projection.space = trackingEnabled && localSpace_ ? localSpace_ : viewSpace_;
            projection.viewCount = static_cast<std::uint32_t>(projectionViews.size());
            projection.views = projectionViews.data();
        }
        // Retained-pair fallback: a failed locate or pre-copy
        // bridge wait must not turn an otherwise valid world into a black blink.
        // No swapchain work, no retagging, and respect shouldRender=false.
        if (aerEnabled && frameState.shouldRender && !submit &&
            !eyeSwapchainsTouched_ && aerProjectionHistory_.Pair(projectionViews))
        {
            projection.space = trackingEnabled && localSpace_ ? localSpace_ : viewSpace_;
            projection.viewCount = static_cast<std::uint32_t>(projectionViews.size());
            projection.views = projectionViews.data();
            submit = true;
            ++aerRescuedFrames_;
        }
        if (aerEnabled && frameState.shouldRender)
        {
            if (!submit) ++aerBlackFrames_;
            if ((++aerSubmitReports_ % 600) == 0)
                XrLog("[AERSUB] retainedRescue=%llu black=%llu; FIFO owns eye, draw owns pose",
                    static_cast<unsigned long long>(aerRescuedFrames_),
                    static_cast<unsigned long long>(aerBlackFrames_));
        }
        // [HUDLAYER] Two interface layers over the projection, premultiplied
        // alpha ([HUDALPHA] coverage rule per blend state):
        //  - PANEL: one VIEW-space head-locked quad, both eyes, user-sized -
        //    everything screen-space (bars, minimap, menus, text).
        //  - WORLD ([UIWORLD]/[HUDLENS]): the draws that move with the camera
        //    (lock-on reticle, objective and enemy markers) on one quad PER
        //    EYE, in the projection's space, at that eye's rendered pose,
        //    sized to the tan-space extent of the lens claimed for that eye
        //    (convergence shift included), so a marker texel sits in exactly
        //    the direction the game drew it over the world.
        std::array<XrCompositionLayerQuad, 4+ObjectiveWorld::MaxMarkers> hudQuads{};
        XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        std::array<const XrCompositionLayerBaseHeader*, 6+ObjectiveWorld::MaxMarkers> layers{};
        std::uint32_t layerCount = 0;
        if (submit)
        {
            layers[layerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            const float distance = std::clamp(
                g_hudDistance.load(std::memory_order_relaxed), 0.3f, 8.0f);
            if (hudReady && hud_.ready && hud_.w && hud_.h)
            {
                auto& hudQuad = hudQuads[0];
                hudQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hudQuad.subImage.swapchain = hud_.swapchain;
                hudQuad.subImage.imageRect.offset = {0, 0};
                hudQuad.subImage.imageRect.extent = {
                    static_cast<std::int32_t>(hud_.w), static_cast<std::int32_t>(hud_.h)};
                hudQuad.subImage.imageArrayIndex = 0;
                const float width = std::clamp(
                    g_hudWidth.load(std::memory_order_relaxed), 0.2f, 6.0f);
                hudQuad.space = viewSpace_;
                hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudQuad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                hudQuad.pose.position = {0.0f,
                    g_hudYOffset.load(std::memory_order_relaxed), -distance};
                hudQuad.size = {width, width * static_cast<float>(hud_.h) /
                    static_cast<float>(hud_.w)};
                layers[layerCount++] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
            }
            if (hudWorldReady && hudWorld_.ready && hudWorld_.w && hudWorld_.h)
            {
                // One quad, both eyes, at the head-centre rendered pose, on
                // the lens the game rendered (tan extents averaged over the
                // two eyes' claims, which cancels the convergence shift).
                auto& hudQuad = hudQuads[1];
                hudQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                hudQuad.subImage.swapchain = hudWorld_.swapchain;
                hudQuad.subImage.imageRect.offset = {0, 0};
                hudQuad.subImage.imageRect.extent = {
                    static_cast<std::int32_t>(hudWorld_.w), static_cast<std::int32_t>(hudWorld_.h)};
                hudQuad.subImage.imageArrayIndex = 0;
                const float worldDistance = std::clamp(
                    g_hudWorldDistance.load(std::memory_order_relaxed), 1.0f, 100.0f);
                const auto& p0 = projectionViews[0];
                const auto& p1 = projectionViews[1];
                float tl = 0.5f * (std::tan(p0.fov.angleLeft) + std::tan(p1.fov.angleLeft));
                float tr = 0.5f * (std::tan(p0.fov.angleRight) + std::tan(p1.fov.angleRight));
                float td = 0.5f * (std::tan(p0.fov.angleDown) + std::tan(p1.fov.angleDown));
                float tu = 0.5f * (std::tan(p0.fov.angleUp) + std::tan(p1.fov.angleUp));
                // No UI-lens override: in stereo the real marker transforms
                // measure P00 0.593 / P11 1.054 = the rendered lens, so
                // markers sit on the eye claim (uiLens is still measured and
                // logged for checking).
                const XrVector3f local{worldDistance * (tl + tr) * 0.5f,
                    worldDistance * (td + tu) * 0.5f, -worldDistance};
                XrQuaternionf worldQ = p0.pose.orientation;
                if (g_hudWorldBaseCamera.load(std::memory_order_relaxed))
                {
                    if (projection.space == localSpace_ && localSpace_ != XR_NULL_HANDLE)
                    {
                        AcquireSRWLockShared(&g_headPose.lock);
                        const float cy = g_headPose.centerYaw;
                        const float cp = g_headPose.centerPitch;
                        ReleaseSRWLockShared(&g_headPose.lock);
                        worldQ = HeadPoseMath::YawPitch(cy, cp);
                    }
                    else
                        worldQ = {0.0f, 0.0f, 0.0f, 1.0f};
                }
                const XrVector3f offset = RotateByQuaternion(worldQ, local);
                const XrVector3f centre{0.5f * (p0.pose.position.x + p1.pose.position.x),
                    0.5f * (p0.pose.position.y + p1.pose.position.y),
                    0.5f * (p0.pose.position.z + p1.pose.position.z)};
                hudQuad.space = projection.space;
                hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                hudQuad.pose.orientation = worldQ;
                hudQuad.pose.position = {centre.x + offset.x, centre.y + offset.y, centre.z + offset.z};
                hudQuad.size = {worldDistance * (tr - tl), worldDistance * (tu - td)};
                layers[layerCount++] =
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
            }
            // [AERMARKER] AER has no native render bank; its world-icon camera
            // is the committed view + render pose of the newest captured eye.
            if (aerEnabled && !objectiveCameraValid && aerMarkerValid_) {
                AcquireSRWLockShared(&g_headPose.lock);
                const bool sameEpoch = aerMarkerEpoch_ == g_headPoseEpoch;
                ReleaseSRWLockShared(&g_headPose.lock);
                if (sameEpoch) { objectiveCamera = aerMarker_; objectiveCameraValid = true; }
            }
            if (!aerEnabled) aerMarkerValid_ = false;
            for(unsigned slot=0;slot<ObjectiveWorld::MaxMarkers;++slot) {
                auto& objective_=objectives_[slot];const auto& objectiveFrame=g_objectiveFrames[slot];
                if(objectiveReady[slot] && objectiveFrame.draws && objective_.ready && !reuseHeldImage)
                {
                    const auto& eye=projectionViews[0];
                    auto& quad=hudQuads[2+slot];quad={XR_TYPE_COMPOSITION_LAYER_QUAD};
                    quad.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    quad.space=projection.space;quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain=objective_.swapchain;
                    quad.subImage.imageRect.extent={ObjectiveWorld::TextureSize,ObjectiveWorld::TextureSize};
                    quad.pose.orientation=eye.pose.orientation;
                    float view[3]{};
                    const bool world=objectiveCameraValid && ObjectiveWorld::ViewPoint(
                        objectiveFrame.anchor.point,objectiveCamera.markerView,view);
                    XrVector3f local{};
                    XrVector3f origin=eye.pose.position;
                    if(world) {
                        // Unproject through the SAME head sample that produced markerView.
                        // The projection layer may intentionally use a newer timing tag;
                        // using that tag here would add head motion to a static world point.
                        origin=objectiveCamera.poses[0].position;
                        quad.pose.orientation=objectiveCamera.poses[0].orientation;
                        const float compositionScale=ObjectiveWorld::CompositionScale(view);
                        local={view[0]*compositionScale,view[1]*compositionScale,view[2]*compositionScale};
                        const float width=ObjectiveWorld::ReadableQuadMeters(std::hypot(view[0],view[1],view[2]))*compositionScale*
                            ObjectiveWorld::TexelsPerCanvasPixel/objectiveFrame.texelsPerCanvasPixel *
                            (40.f/std::fmax(4.f,objectiveFrame.canvasDiameter));
                        quad.size={width,width};
                    } else {
                        // Keep a readable original-position fallback if the paired camera is unavailable.
                        // This is logged separately and never presented as true world placement.
                        const auto& other=projectionViews[1];
                        const float tl=.5f*(std::tan(eye.fov.angleLeft)+std::tan(other.fov.angleLeft));
                        const float tr=.5f*(std::tan(eye.fov.angleRight)+std::tan(other.fov.angleRight));
                        const float td=.5f*(std::tan(eye.fov.angleDown)+std::tan(other.fov.angleDown));
                        const float tu=.5f*(std::tan(eye.fov.angleUp)+std::tan(other.fov.angleUp));
                        const float d=g_hudWorldDistance.load(std::memory_order_relaxed);
                        local={d*(tl+(tr-tl)*objectiveFrame.anchor.screen[0]/1920),
                               d*(tu-(tu-td)*objectiveFrame.anchor.screen[1]/1080),-d};
                        origin={.5f*(eye.pose.position.x+other.pose.position.x),.5f*(eye.pose.position.y+other.pose.position.y),.5f*(eye.pose.position.z+other.pose.position.z)};
                        const float canvas=ObjectiveWorld::TextureSize/objectiveFrame.texelsPerCanvasPixel;
                        quad.size={d*(tr-tl)*canvas/1920,d*(tu-td)*canvas/1080};
                    }
                    const auto offset=RotateByQuaternion(quad.pose.orientation,local);
                    quad.pose.position={origin.x+offset.x,origin.y+offset.y,origin.z+offset.z};
                    if(slot==0 && objectivePixels_.queuedFrame==objective_.submits) {
                        const XrQuaternionf inverse{-eye.pose.orientation.x,-eye.pose.orientation.y,-eye.pose.orientation.z,eye.pose.orientation.w};
                        const XrVector3f delta{quad.pose.position.x-eye.pose.position.x,quad.pose.position.y-eye.pose.position.y,quad.pose.position.z-eye.pose.position.z};
                        const auto relative=RotateByQuaternion(inverse,delta);
                        XrLog("[OBJECTIVEPIXELSPOSE] frame=%llu mode=%s space=%s serial=%llu anchor=%.6f/%.6f/%.6f canvas=%.3f/%.3f view=%.6f/%.6f/%.6f xr=%.6f/%.6f/%.6f q=%.6f/%.6f/%.6f/%.6f eyeRelative=%.6f/%.6f/%.6f size=%.6f/%.6f fov=%.6f/%.6f/%.6f/%.6f",
                            objective_.submits,world?"world":objectiveCameraValid?"behind":"fallback",projection.space==localSpace_?"local":"view",objectiveCamera.serial,
                            objectiveFrame.anchor.point[0],objectiveFrame.anchor.point[1],objectiveFrame.anchor.point[2],objectiveFrame.anchor.screen[0],objectiveFrame.anchor.screen[1],
                            view[0],view[1],view[2],quad.pose.position.x,quad.pose.position.y,quad.pose.position.z,
                            quad.pose.orientation.x,quad.pose.orientation.y,quad.pose.orientation.z,quad.pose.orientation.w,
                            relative.x,relative.y,relative.z,quad.size.width,quad.size.height,eye.fov.angleLeft,eye.fov.angleRight,eye.fov.angleUp,eye.fov.angleDown);
                    }
                    if(world || !objectiveCameraValid)
                        layers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                    static unsigned reports[ObjectiveWorld::MaxMarkers]{};
                    if((reports[slot]++%300)==0) XrLog("[OBJECTIVE3D] slot=%u mode=%s id=%llX draws=%u point=%.3f/%.3f/%.3f view=%.3f/%.3f/%.3f cameraSerial=%llu ringAngle=1.5-to-1.75deg compositionRangeMax=20m",slot,world?"world":objectiveCameraValid?"behind-camera":"camera-fallback",
                        static_cast<unsigned long long>(objectiveFrame.anchor.id),objectiveFrame.draws,
                        objectiveFrame.anchor.point[0],objectiveFrame.anchor.point[1],objectiveFrame.anchor.point[2],
                        view[0],view[1],view[2],static_cast<unsigned long long>(objectiveCamera.serial));
                }
            }
            if(targetReady && targetFrame.draws && target_.ready && !reuseHeldImage && objectiveCameraValid) {
                float v[3]{};
                if(ObjectiveWorld::ViewPoint(targetFrame.anchor.point,objectiveCamera.markerView,v)) {
                    auto& quad=hudQuads[2+ObjectiveWorld::MaxMarkers];quad={XR_TYPE_COMPOSITION_LAYER_QUAD};
                    quad.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    quad.space=projection.space;quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain=target_.swapchain;quad.subImage.imageRect.extent={512,512};
                    quad.pose.orientation=objectiveCamera.poses[0].orientation;
                    const float scale=ObjectiveWorld::CompositionScale(v);
                    const float distance=std::hypot(v[0],v[1],v[2])*scale;
                    // Cancel native approach/animation scale using measured opposite-corner spacing.
                    const float pixelMeters=distance*.03491013f/targetFrame.canvasDiameter;
                    XrVector3f local{v[0]*scale+targetFrame.captureOffset[0]*pixelMeters,
                        v[1]*scale-targetFrame.captureOffset[1]*pixelMeters,v[2]*scale};
                    const auto offset=RotateByQuaternion(quad.pose.orientation,local);
                    const auto origin=objectiveCamera.poses[0].position;
                    quad.pose.position={origin.x+offset.x,origin.y+offset.y,origin.z+offset.z};
                    const float width=512*pixelMeters/targetFrame.texelsPerCanvasPixel;
                    quad.size={width,width};
                    layers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                }
            }
            if(interactionReady && interactionFrame.draws && interaction_.ready && !reuseHeldImage && objectiveCameraValid) {
                float v[3]{};
                if(ObjectiveWorld::ViewPoint(interactionFrame.anchor.point,objectiveCamera.markerView,v)) {
                    auto& quad=hudQuads[3+ObjectiveWorld::MaxMarkers];quad={XR_TYPE_COMPOSITION_LAYER_QUAD};
                    quad.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    quad.space=projection.space;quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain=interaction_.swapchain;quad.subImage.imageRect.extent={512,512};
                    quad.pose.orientation=objectiveCamera.poses[0].orientation;
                    const float scale=ObjectiveWorld::CompositionScale(v);
                    const float distance=std::hypot(v[0],v[1],v[2])*scale;
                    // Normalize the interaction ring to a bounded angular size.
                    const float pixelMeters=distance*.03054564f/interactionFrame.canvasDiameter;
                    XrVector3f local{v[0]*scale+interactionFrame.captureOffset[0]*pixelMeters,
                        v[1]*scale-interactionFrame.captureOffset[1]*pixelMeters,v[2]*scale};
                    const auto offset=RotateByQuaternion(quad.pose.orientation,local);
                    const auto origin=objectiveCamera.poses[0].position;
                    quad.pose.position={origin.x+offset.x,origin.y+offset.y,origin.z+offset.z};
                    const float width=512*pixelMeters/interactionFrame.texelsPerCanvasPixel;
                    quad.size={width,width};
                    layers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                }
            }
            if (menuReady && menu_.ready && menu_.w && menu_.h)
            {
                // [MENUQUAD] last layer = on top of everything, 1.2 m ahead;
                // width from the menu's size slider (default 0.70 m, about
                // 32 degrees).
                const std::int32_t mx = std::clamp(g_menuRect[0].load(std::memory_order_relaxed), 0, static_cast<std::int32_t>(menu_.w) - 16);
                const std::int32_t my = std::clamp(g_menuRect[1].load(std::memory_order_relaxed), 0, static_cast<std::int32_t>(menu_.h) - 16);
                const std::int32_t mw = std::clamp(g_menuRect[2].load(std::memory_order_relaxed), 16, static_cast<std::int32_t>(menu_.w) - mx);
                const std::int32_t mh = std::clamp(g_menuRect[3].load(std::memory_order_relaxed), 16, static_cast<std::int32_t>(menu_.h) - my);
                menuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                menuQuad.space = viewSpace_;
                menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                menuQuad.subImage.swapchain = menu_.swapchain;
                menuQuad.subImage.imageRect.offset = {mx, my};
                menuQuad.subImage.imageRect.extent = {mw, mh};
                menuQuad.subImage.imageArrayIndex = 0;
                menuQuad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                menuQuad.pose.position = {g_menuOffsetX.load(std::memory_order_relaxed),
                    g_menuOffsetY.load(std::memory_order_relaxed), -1.2f};   // [MENUDRAG]
                const float width = std::clamp(g_menuWidth.load(std::memory_order_relaxed), 0.3f, 1.6f);
                menuQuad.size = {width, width * static_cast<float>(mh) / static_cast<float>(mw)};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
                if ((++menuReports_ % 900) == 1)
                    XrLog("[MENUQUAD] menu panel rect=%dx%d+%d+%d of %ux%u, %.2fx%.2fm at 1.2m",
                        mw, mh, mx, my, menu_.w, menu_.h, menuQuad.size.width, menuQuad.size.height);
            }
            if ((++hudLensReports_ % 1800) == 1)
                XrLog("[HUDLENS] panel=%s world=%s(%s) layers=%u panelSize=%.2fx%.2fm worldSize=%.2fx%.2fm at %.2fm "
                      "uiLens=%.3f/%.3f (eye claim L/R/U/D=%.1f/%.1f/%.1f/%.1f deg)",
                    hudReady ? "on" : "off", hudWorldReady ? "on" : "off",
                    g_hudWorldBaseCamera.load(std::memory_order_relaxed) ? "base-camera" : "head", layerCount,
                    hudQuads[0].size.width, hudQuads[0].size.height,
                    hudQuads[1].size.width, hudQuads[1].size.height,
                    g_hudWorldDistance.load(std::memory_order_relaxed),
                    g_uiLensP00.load(std::memory_order_relaxed), g_uiLensP11.load(std::memory_order_relaxed),
                    projectionViews[0].fov.angleLeft * 57.2958f,
                    projectionViews[0].fov.angleRight * 57.2958f,
                    projectionViews[0].fov.angleUp * 57.2958f,
                    projectionViews[0].fov.angleDown * 57.2958f);
        }
        // Built after every HUD quad above: those size themselves from the
        // FULL rendered lens, which the crop below must not narrow.
        // [STEAMVRFOV] kept OFF: SteamVR shows the full
        // uncropped view, and the crop removed the spare edge its reprojection
        // uses (black edges on head turns). Set cropToFrustum_ to re-enable.
        if (isSteamVr_ && submit && cropToFrustum_)
            CropToRuntimeFrustum(projectionViews);
        if (submit && isMeta_)
            FitToMetaFrustum(projectionViews);   // [METAFIT]
        XrCompositionLayerProjection blackProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::array<XrCompositionLayerProjectionView, 2> blackViews{};
        // [STEAMVRSCENE] SteamVR is a scene-app state machine: with no
        // projection layer it brings its waiting room back over the headset.
        // Every frame that would go out empty (before the first capture, a
        // failed transfer) gets a 64px opaque-black projection instead.
        // SteamVR only.
        if (isSteamVr_ && frameState.shouldRender && layerCount == 0 &&
            runtimeEyeFovValid_ && EnsureBlackScene())
        {
            for (std::size_t eye = 0; eye < blackViews.size(); ++eye)
            {
                auto& view = blackViews[eye];
                view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose = views_[eye].pose;
                view.fov = runtimeEyeFov_[eye];
                view.subImage.swapchain = blackSwapchain_;
                view.subImage.imageRect.offset = {0, 0};
                view.subImage.imageRect.extent = {kBlackSceneSize, kBlackSceneSize};
                view.subImage.imageArrayIndex = 0;
            }
            blackProjection.space = trackingEnabled && localSpace_ ? localSpace_ : viewSpace_;
            blackProjection.viewCount = static_cast<std::uint32_t>(blackViews.size());
            blackProjection.views = blackViews.data();
            layers[layerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&blackProjection);
            if ((++blackSceneFrames_ % 900) == 1)
                XrLog("[STEAMVRSCENE] black scene layer submitted for an empty frame (count=%llu)",
                    static_cast<unsigned long long>(blackSceneFrames_));
        }
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = layerCount;
        endInfo.layers = layerCount ? layers.data() : nullptr;
        result = PresentTiming::Measure(PresentTiming::XrEnd,[&]{return xrEndFrame_(session_, &endInfo);});
        if (XR_SUCCEEDED(result) && submit && holdRequested && nativeOnly)
        {
            if (!reuseHeldImage)
            {
                AcquireSRWLockShared(&g_headPose.lock);
                const auto capturedEpoch = g_headPoseEpoch;
                ReleaseSRWLockShared(&g_headPose.lock);
                imageHold_.Capture(projectionViews, localSpace_, capturedEpoch);
                XrLog("[IMAGEHOLD] captured fixed pair: pose/FOV/rect/space held together; "
                    "game keeps rendering, eye image transfers suspended; not a fix");
            }
            else if ((++imageHoldFrames_ % 300) == 1)
                XrLog("[IMAGEHOLD] reusing fixed pair submissions=%llu; no eye copies/releases",
                    static_cast<unsigned long long>(imageHoldFrames_));
        }
        if (XR_FAILED(result))
            XrLog("xrEndFrame failed result=%d", result);

        if (submit && nativeStereoEnabled && (submittedFrames_ % 600) == 0)
        {
            constexpr float kDeg = 180.0f / kPi;
            XrLog("[SPLITFOV] rect=%dx%d+%d+%d fill=%.2f claim L/R/U/D=%.1f/%.1f/%.1f/%.1f deg "
                  "focal=%.3f/%.3f swap=%d convergence=%.4f",
                projectionViews[0].subImage.imageRect.extent.width,
                projectionViews[0].subImage.imageRect.extent.height,
                projectionViews[0].subImage.imageRect.offset.x,
                projectionViews[0].subImage.imageRect.offset.y,
                g_splitFill.load(std::memory_order_relaxed),
                projectionViews[0].fov.angleLeft * kDeg,
                projectionViews[0].fov.angleRight * kDeg,
                projectionViews[0].fov.angleUp * kDeg,
                projectionViews[0].fov.angleDown * kDeg,
                g_gameFocalX.load(std::memory_order_relaxed),
                g_gameFocalY.load(std::memory_order_relaxed),
                g_splitSwap.load(std::memory_order_relaxed) ? 1 : 0,
                g_splitConvergence.load(std::memory_order_relaxed));
        }
        const int submittedMode = aerEnabled ? 2 : (nativeStereoEnabled ? 1 : 0);
        const bool modeChanged = submit && submittedMode != lastSubmittedMode_;
        if (submit)
            lastSubmittedMode_ = submittedMode;
        const std::uint64_t frame = submit ? ++submittedFrames_ : submittedFrames_;
        if (submit && (frame <= 4 || modeChanged || (frame % 600) == 0))
            XrLog("submitted %s frame=%llu rects=%d+%dx%d,%d+%dx%d "
                "fill=%.3f convergence=%.4f nextAerEye=%d",
                aerEnabled ? "AER" : (nativeStereoEnabled ? "Lane B SBS" : "MONO"),
                static_cast<unsigned long long>(frame),
                projectionViews[0].subImage.imageRect.offset.x,
                projectionViews[0].subImage.imageRect.extent.width,
                projectionViews[0].subImage.imageRect.extent.height,
                projectionViews[1].subImage.imageRect.offset.x,
                projectionViews[1].subImage.imageRect.extent.width,
                projectionViews[1].subImage.imageRect.extent.height,
                aerEnabled ? g_fillFactor.load(std::memory_order_relaxed)
                    : (nativeStereoEnabled ? g_splitFill.load(std::memory_order_relaxed) : 1.0f),
                aerEnabled ? g_aerConvergence.load(std::memory_order_relaxed) : 0.0f,
                g_aerRenderEye.load(std::memory_order_relaxed));
    }
    void ReleaseD3DResources()
    {
        NativeDrawPose::SetActive(false);
        imageHold_.Reset();
        aerProjectionHistory_.Reset();
        singlePoseContinuity_.Reset();
        imageHoldFrames_ = 0;
        g_nativeImageHold.store(false);
        FullEye::Reset();
        SafeRelease(fullEyeMutexXr_);SafeRelease(fullEyeSharedXr_);SafeRelease(fullEyeMutexGame_);SafeRelease(fullEyeSharedGame_);
        if(fullEyeHandle_)CloseHandle(fullEyeHandle_);fullEyeHandle_=nullptr;fullEyeReady_=fullEyeFailed_=fullEyeFrame_=false;
        sharedReady_ = false;
        SafeRelease(sharedMutexXr_);
        SafeRelease(sharedTextureXr_);
        SafeRelease(sharedMutexGame_);
        SafeRelease(sharedTextureGame_);
        if (sharedHandle_ && sharedHandleIsNt_)
            CloseHandle(sharedHandle_);
        sharedHandle_ = nullptr;
        sharedHandleIsNt_ = false;
        SafeRelease(sourceView_);
        SafeRelease(sourceTexture_);
        for (auto*& buffer : constantBuffers_)
            SafeRelease(buffer);
        SafeRelease(sampler_);
        ReleaseSharpen();
        SafeRelease(pixelShader_);
        SafeRelease(vertexShader_);
        SafeRelease(deferredContext_);
        SafeRelease(immediateContext_);
        SafeRelease(device_);
        SafeRelease(gameImmediateContext_);
        SafeRelease(gameDevice_);
        if (realD3D11_)
            FreeLibrary(realD3D11_);
        realD3D11_ = nullptr;
        if (realDxgi_)
            FreeLibrary(realDxgi_);
        realDxgi_ = nullptr;
        sourceWidth_ = 0;
        sourceHeight_ = 0;
        sourceFormat_ = DXGI_FORMAT_UNKNOWN;
    }

    void ResetFunctions()
    {
        xrGetInstanceProcAddr_ = nullptr;
        xrEnumerateInstanceExtensionProperties_ = nullptr;
        xrCreateInstance_ = nullptr;
        xrDestroyInstance_ = nullptr;
        xrGetSystem_ = nullptr;
        xrGetD3D11GraphicsRequirementsKHR_ = nullptr;
        xrCreateSession_ = nullptr;
        xrDestroySession_ = nullptr;
        xrCreateReferenceSpace_ = nullptr;
        xrDestroySpace_ = nullptr;
        xrEnumerateViewConfigurationViews_ = nullptr;
        xrEnumerateSwapchainFormats_ = nullptr;
        xrCreateSwapchain_ = nullptr;
        xrDestroySwapchain_ = nullptr;
        xrEnumerateSwapchainImages_ = nullptr;
        xrPollEvent_ = nullptr;
        xrBeginSession_ = nullptr;
        xrEndSession_ = nullptr;
        xrWaitFrame_ = nullptr;
        xrBeginFrame_ = nullptr;
        xrLocateViews_ = nullptr;
        xrAcquireSwapchainImage_ = nullptr;
        xrWaitSwapchainImage_ = nullptr;
        xrReleaseSwapchainImage_ = nullptr;
        xrEndFrame_ = nullptr;
    }

    HMODULE loader_{};
    bool loaderPinned_{};
    std::atomic_bool initializationStarted_{};
    std::atomic_bool ready_{};
    std::atomic_uint64_t nextRetryMs_{};
    XrInstance instance_{XR_NULL_HANDLE};
    XrSystemId systemId_{XR_NULL_SYSTEM_ID};
    XrSession session_{XR_NULL_HANDLE};
    XrSpace viewSpace_{XR_NULL_HANDLE};
    XrSpace localSpace_{XR_NULL_HANDLE};
    XrSessionState sessionState_{XR_SESSION_STATE_UNKNOWN};
    bool sessionRunning_{};
    std::uint64_t submittedFrames_{};
    std::uint64_t frameAttempts_{};
    // [PACE] cadence instrument only: the runtime's display period and a
    // histogram of present intervals in display periods.  There is no
    // in-Present pacer: sleeping inside Present breaks the FOV pin (the
    // engine lens alternates 87/48 degrees every frame).
    XrDuration displayPeriodNs_{};
    std::vector<XrView> tagViews_{};
    bool tagViewsValid_{};
    long long cadenceLastQpc_{};
    std::uint64_t cadenceCount_{};
    double cadenceSumMs_{};
    double cadenceMinMs_{1e9};
    double cadenceMaxMs_{};
    std::array<std::uint32_t, 5> cadenceBuckets_{};

    static long long QpcNow()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }
    static long long QpcFrequency()
    {
        static const long long frequency = []
        {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value.QuadPart > 0 ? value.QuadPart : 1LL;
        }();
        return frequency;
    }

    // Measure the interval between successive presents and report it every
    // 300 frames, in milliseconds and in display periods.
    void NoteCadence(float lookahead)
    {
        const long long now = QpcNow();
        if (cadenceLastQpc_)
        {
            const double intervalMs = static_cast<double>(now - cadenceLastQpc_) * 1000.0 /
                static_cast<double>(QpcFrequency());
            ++cadenceCount_;
            cadenceSumMs_ += intervalMs;
            cadenceMinMs_ = std::min(cadenceMinMs_, intervalMs);
            cadenceMaxMs_ = std::max(cadenceMaxMs_, intervalMs);
            const double periodMs = displayPeriodNs_ > 0
                ? static_cast<double>(displayPeriodNs_) / 1e6 : 0.0;
            const int bucket = periodMs > 0.0
                ? std::clamp(static_cast<int>(std::lround(intervalMs / periodMs)), 0, 4) : 0;
            ++cadenceBuckets_[static_cast<std::size_t>(bucket)];
            if (cadenceCount_ % 300 == 0)
            {
                const double meanMs = cadenceSumMs_ / static_cast<double>(cadenceCount_);
                XrLog("[PACE] display=%.2fms (%.1fHz) presents: mean=%.2fms (%.1f/s) "
                    "min=%.2f max=%.2f; per display period 0:%u 1:%u 2:%u 3:%u 4+:%u; "
                    "lookahead=%.2f frames depth=%d",
                    periodMs, periodMs > 0.0 ? 1000.0 / periodMs : 0.0, meanMs,
                    meanMs > 0.0 ? 1000.0 / meanMs : 0.0, cadenceMinMs_, cadenceMaxMs_,
                    cadenceBuckets_[0], cadenceBuckets_[1], cadenceBuckets_[2],
                    cadenceBuckets_[3], cadenceBuckets_[4], lookahead,
                    g_headPipelineDepth.load(std::memory_order_relaxed));
                cadenceCount_ = 0;
                cadenceSumMs_ = 0.0;
                cadenceMinMs_ = 1e9;
                cadenceMaxMs_ = 0.0;
                cadenceBuckets_ = {};
            }
        }
        cadenceLastQpc_ = now;
    }
    int lastSubmittedMode_{-1};
    int lastTrackingMode_{-1};
    bool lastAerEnabled_{};
    NativeImageHold imageHold_{};
    AerProjectionHistory aerProjectionHistory_{};
    SinglePoseContinuity singlePoseContinuity_{};
    bool singlePoseGuardEnabled_{};
    bool nativeDrawPoseConsumed_{};
    std::uint64_t singlePoseHolds_{};
    bool eyeSwapchainsTouched_{};
    std::uint64_t aerRescuedFrames_{}, aerBlackFrames_{}, aerSubmitReports_{};
    NativeDrawPose::Pose nativeDrawPose_{};
    std::uint64_t aerPoseRoutes_[2]{};
    std::uint64_t imageHoldFrames_{};
    std::array<bool, 2> aerImageValid_{};
    std::array<bool, 2> aerPoseValid_{};
    std::uint64_t aerLastSeq_{};
    std::uint64_t aerCaptures_{};
    std::uint64_t aerRepeats_{};
    std::uint64_t aerMaxSeqGap_{};
    std::uint64_t aerStallPresents_{};
    std::uint32_t aerSameEyeTwice_{};
    std::uint32_t aerResyncs_{};
    int aerPrevCaptureEye_{-1};
    std::uint64_t aerTagReports_{};
    std::array<RenderedHeadPose, 2> aerRenderedHeadPoses_{};
    std::uint64_t aerTagExact_{}, aerTagLatest_{}, aerTagAge_[5]{};   // [AERTAG]
    std::uint64_t aerTagRendered_{}, aerPair_[4]{};                   // [RENDERCAM]
    // [WORLDLOCK] the drawn camera of the image eye 0 shows, and jitter series
    float drawnView_[16]{};
    bool drawnValid_{};
    std::uint64_t drawnPrevSerial_{}, drawnSteps_[4]{};
    WorldLock::Series worldShown_, worldTag_, worldHead_;
    int worldMode_{-1};
    std::uint64_t worldLogs_{};
    std::uint64_t aerTagCurrent_{};                                   // [TAGALL]
    bool dockValid_{};                                                // [DOCK]
    std::uint64_t dockEpoch_{};
    XrQuaternionf dockOrientation_{0.0f, 0.0f, 0.0f, 1.0f};
    RenderedHeadPose aerMarker_{};                                     // [AERMARKER]
    std::uint64_t aerMarkerEpoch_{}, aerMarkerOk_{}, aerMarkerBad_{};
    bool aerMarkerValid_{};
    std::int64_t swapchainFormat_{};
    // SteamVR-only state ([XRRUNTIME], [STEAMVRFOV], [STEAMVRSCENE]).
    static constexpr std::int32_t kBlackSceneSize = 64;
    bool isSteamVr_{};
    bool isMeta_{};   // [METAFIT]
    std::array<XrFovf, 2> runtimeEyeFov_{};
    bool runtimeEyeFovValid_{};
    XrSwapchain blackSwapchain_{XR_NULL_HANDLE};
    bool blackSceneTried_{};
    std::uint64_t blackSceneFrames_{};
    std::uint64_t fovCropReports_{};
    bool cropToFrustum_{false};
    std::array<EyeSwapchain, 2> eyes_{};
    std::vector<XrView> views_;

    ID3D11Texture2D* fullEyeSharedGame_{};
    IDXGIKeyedMutex* fullEyeMutexGame_{};
    ID3D11Texture2D* fullEyeSharedXr_{};
    IDXGIKeyedMutex* fullEyeMutexXr_{};
    HANDLE fullEyeHandle_{};
    bool fullEyeReady_{},fullEyeFailed_{},fullEyeFrame_{};
    unsigned fullEyeWidth_{},fullEyeHeight_{},fullEyePendingWidth_{},fullEyePendingHeight_{};
    bool aerPairStageValid_{};
    bool aerPairStagedThisFrame_{};
    bool aerPairReleasedThisFrame_{};
    std::uint64_t aerPairsStaged_{}, aerPairsReleased_{};
    bool videoModeLogged_{};
    int videoLayoutLogged_{};
    std::uint64_t videoFrames_{};

    // [AFW] second cross-device bridge carrying the synthesized eye.
    ID3D11Texture2D* afwSharedGame_{};
    IDXGIKeyedMutex* afwMutexGame_{};
    ID3D11Texture2D* afwSharedXr_{};
    IDXGIKeyedMutex* afwMutexXr_{};
    HANDLE afwHandle_{};
    bool afwReady_{};
    bool afwFailed_{};
    bool afwSynthesizedThisFrame_{};

    // [HUDLAYER] two bridges: the panel (all screen-space interface) and
    // the world layer (draws that move with the camera: markers).
    std::vector<std::int64_t> offeredFormats_;
    HudBridge hud_{};
    HudBridge hudWorld_{"world"};
    HudBridge objectives_[ObjectiveWorld::MaxMarkers]{{"objective-0"},{"objective-1"},{"objective-2"},{"objective-3"}};
    HudBridge target_{"target-3d"};
    HudBridge interaction_{"interaction-3d"};
    HudBridge menu_{"menu"};
    std::uint64_t menuReports_{};
    ObjectivePixelProbe::Capture objectivePixels_;
    std::uint64_t hudLensReports_{};

    ID3D11Device* device_{};
    ID3D11DeviceContext* immediateContext_{};
    ID3D11Device* gameDevice_{};
    std::uint64_t traceCapture_{};
    ID3D11DeviceContext* gameImmediateContext_{};
    ID3D11Texture2D* sharedTextureGame_{};
    IDXGIKeyedMutex* sharedMutexGame_{};
    ID3D11Texture2D* sharedTextureXr_{};
    IDXGIKeyedMutex* sharedMutexXr_{};
    HANDLE sharedHandle_{};
    bool sharedHandleIsNt_{};
    bool sharedReady_{};
    HMODULE realDxgi_{};
    HMODULE realD3D11_{};
    ID3D11DeviceContext* deferredContext_{};
    ID3D11VertexShader* vertexShader_{};
    ID3D11PixelShader* pixelShader_{};
    ID3D11SamplerState* sampler_{};
    std::array<ID3D11Buffer*, 2> constantBuffers_{};
    ID3D11Texture2D* sourceTexture_{};
    ID3D11ShaderResourceView* sourceView_{};
    UINT sourceWidth_{};
    UINT sourceHeight_{};
    DXGI_FORMAT sourceFormat_{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT desktopFormat_{DXGI_FORMAT_UNKNOWN};
    UINT desktopWidth_{};
    UINT desktopHeight_{};

    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_{};
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties_{};
    PFN_xrCreateInstance xrCreateInstance_{};
    PFN_xrDestroyInstance xrDestroyInstance_{};
    PFN_xrGetSystem xrGetSystem_{};
    PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR_{};
    PFN_xrCreateSession xrCreateSession_{};
    PFN_xrDestroySession xrDestroySession_{};
    PFN_xrCreateReferenceSpace xrCreateReferenceSpace_{};
    PFN_xrDestroySpace xrDestroySpace_{};
    PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews_{};
    PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats_{};
    PFN_xrCreateSwapchain xrCreateSwapchain_{};
    PFN_xrDestroySwapchain xrDestroySwapchain_{};
    PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages_{};
    PFN_xrPollEvent xrPollEvent_{};
    PFN_xrBeginSession xrBeginSession_{};
    PFN_xrEndSession xrEndSession_{};
    PFN_xrWaitFrame xrWaitFrame_{};
    PFN_xrBeginFrame xrBeginFrame_{};
    PFN_xrLocateViews xrLocateViews_{};
    PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage_{};
    PFN_xrWaitSwapchainImage xrWaitSwapchainImage_{};
    PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage_{};
    PFN_xrEndFrame xrEndFrame_{};
};

Presenter g_presenter;
} // namespace

namespace RetailXr
{
void OnPresent(IDXGISwapChain* swapChain, bool nativeStereoEnabled, bool aerEnabled)
{
    g_presenter.OnPresent(swapChain, nativeStereoEnabled, aerEnabled);
}

void SetSplitSwapEyes(bool swap)
{
    g_splitSwap.store(swap, std::memory_order_release);
}

bool GetSplitSwapEyes()
{
    return g_splitSwap.load(std::memory_order_acquire);
}

void SetSplitConvergence(float tangentShift)
{
    g_splitConvergence.store(std::clamp(tangentShift, -0.25f, 0.25f),
        std::memory_order_release);
}

float GetSplitConvergence()
{
    return g_splitConvergence.load(std::memory_order_acquire);
}

void SetSplitHonestLens(bool enabled)
{
    g_splitHonestLens.store(enabled, std::memory_order_release);
}

bool GetSplitHonestLens()
{
    return g_splitHonestLens.load(std::memory_order_acquire);
}

void SetMenuLayer(ID3D11Texture2D* texture, bool visible, int x, int y, int w, int h)
{
    g_menuRect[0].store(x, std::memory_order_relaxed);
    g_menuRect[1].store(y, std::memory_order_relaxed);
    g_menuRect[2].store(w, std::memory_order_relaxed);
    g_menuRect[3].store(h, std::memory_order_relaxed);
    g_menuSource.store(texture, std::memory_order_release);
    g_menuVisible.store(visible && texture, std::memory_order_release);
}
void SetMenuWidth(float metres) { g_menuWidth.store(std::clamp(metres, 0.3f, 1.6f), std::memory_order_relaxed); }
float GetMenuWidth() { return g_menuWidth.load(std::memory_order_relaxed); }
unsigned long long GetXrReadyTick() { return g_xrReadyTick.load(std::memory_order_relaxed); }
void SetDockFlatScreens(bool enabled) { g_dockFlatScreens.store(enabled, std::memory_order_relaxed); }
void SetMonoSubmit(bool enabled) { g_monoSubmit.store(enabled, std::memory_order_release); }
bool GetMonoSubmit() { return g_monoSubmit.load(std::memory_order_acquire); }
bool GetDockFlatScreens() { return g_dockFlatScreens.load(std::memory_order_relaxed); }
void SetMenuOffset(float x, float y)
{
    g_menuOffsetX.store(std::clamp(x, -0.8f, 0.8f), std::memory_order_relaxed);
    g_menuOffsetY.store(std::clamp(y, -0.8f, 0.8f), std::memory_order_relaxed);
}
void GetMenuOffset(float* x, float* y)
{
    if (x) *x = g_menuOffsetX.load(std::memory_order_relaxed);
    if (y) *y = g_menuOffsetY.load(std::memory_order_relaxed);
}
bool IsMenuLayerAvailable()
{
    return IsHudLayerConsuming();
}

void SetSharpen(float strength)
{
    g_sharpenStrength.store(std::clamp(strength, 0.0f, 1.0f), std::memory_order_relaxed);
}
float GetSharpen() { return g_sharpenStrength.load(std::memory_order_relaxed); }

void SetVideoScreenFov(float degrees)
{
    g_videoScreenFov.store(std::clamp(degrees, 30.0f, 110.0f), std::memory_order_relaxed);
}
float GetVideoScreenFov() { return VideoScreenFovH(); }

void SetFlatPresents(std::uint32_t presents)
{
    g_flatPresents.store(presents, std::memory_order_relaxed);
}

void SetSplitMonoFallback(bool active)
{
    g_splitMonoFallback.store(active, std::memory_order_release);
}

void SetSplitFill(float fill)
{
    g_splitFill.store(std::clamp(fill, 0.40f, 1.80f), std::memory_order_release);
}

float GetSplitFill()
{
    return g_splitFill.load(std::memory_order_acquire);
}

void SetHudLayer(ID3D11Texture2D* gameTexture, bool hasContent)
{
    g_hudSource.store(gameTexture, std::memory_order_release);
    g_hudHasContent.store(hasContent, std::memory_order_release);
}

void SetHudWorldLayer(ID3D11Texture2D* gameTexture, bool hasContent)
{
    g_hudWorldSource.store(gameTexture, std::memory_order_release);
    g_hudWorldHasContent.store(hasContent, std::memory_order_release);
}

void SetObjectiveLayer(ID3D11Texture2D* texture,const ObjectiveWorld::Frame& frame,unsigned slot)
{
    if(slot>=ObjectiveWorld::MaxMarkers)return;
    g_objectiveFrames[slot]=frame;
    g_objectiveSources[slot].store(texture,std::memory_order_release);
    g_objectiveHasContents[slot].store(frame.draws!=0,std::memory_order_release);
}

void SetInteractionLayer(ID3D11Texture2D* texture,const ObjectiveWorld::Frame& frame) {
    g_interactionFrame=frame;g_interactionSource.store(texture,std::memory_order_release);
    g_interactionHasContent.store(frame.draws!=0,std::memory_order_release);
}
void SetTargetLayer(ID3D11Texture2D* texture,const ObjectiveWorld::Frame& frame) {
    g_targetFrame=frame;g_targetSource.store(texture,std::memory_order_release);
    g_targetHasContent.store(frame.draws!=0,std::memory_order_release);
}

bool IsHudLayerConsuming()
{
    return g_presenter.HudConsuming();
}

void SetHudLayerEnabled(bool enabled)
{
    g_hudLayerEnabled.store(enabled, std::memory_order_release);
}

bool GetHudLayerEnabled()
{
    return g_hudLayerEnabled.load(std::memory_order_acquire);
}

void SetHudWorldBaseCamera(bool enabled)
{
    g_hudWorldBaseCamera.store(enabled, std::memory_order_release);
}

bool GetHudWorldBaseCamera()
{
    return g_hudWorldBaseCamera.load(std::memory_order_acquire);
}

void SetUiLens(float p00, float p11)
{
    g_uiLensP00.store(p00, std::memory_order_relaxed);
    g_uiLensP11.store(p11, std::memory_order_relaxed);
}

void SetHudWorldDistance(float meters)
{
    g_hudWorldDistance.store(std::clamp(meters, 1.0f, 100.0f), std::memory_order_release);
}

float GetHudWorldDistance()
{
    return g_hudWorldDistance.load(std::memory_order_acquire);
}

void SetHudLensExact(bool enabled)
{
    g_hudLensExact.store(enabled, std::memory_order_release);
}

bool GetHudLensExact()
{
    return g_hudLensExact.load(std::memory_order_acquire);
}

void SetHudLayerParams(float distanceMeters, float widthMeters, float verticalOffsetMeters)
{
    g_hudDistance.store(std::clamp(distanceMeters, 0.3f, 8.0f), std::memory_order_release);
    g_hudWidth.store(std::clamp(widthMeters, 0.2f, 6.0f), std::memory_order_release);
    g_hudYOffset.store(std::clamp(verticalOffsetMeters, -2.0f, 2.0f),
        std::memory_order_release);
}

void GetHudLayerParams(float* distanceMeters, float* widthMeters, float* verticalOffsetMeters)
{
    if (distanceMeters)
        *distanceMeters = g_hudDistance.load(std::memory_order_acquire);
    if (widthMeters)
        *widthMeters = g_hudWidth.load(std::memory_order_acquire);
    if (verticalOffsetMeters)
        *verticalOffsetMeters = g_hudYOffset.load(std::memory_order_acquire);
}

int GetAerRenderEye()
{
    return g_aerRenderEye.load(std::memory_order_acquire) & 1;
}

int PeekAerBuildEye()
{
    // Single writer (the camera-build breakpoint handler), so the value Peek
    // returns is the one Publish will stamp.
    return static_cast<int>(
        (g_aerStampSeq.load(std::memory_order_relaxed) + 1) & 1ull);
}

void PublishAerBuildEye(std::uint64_t headSerial, const float* committedView)
{
    const auto seq = g_aerStampSeq.load(std::memory_order_relaxed) + 1;
    const int eye = static_cast<int>(seq & 1ull);
    auto& slot = g_aerRing[seq % kAerRingCount];
    slot.eye.store(eye, std::memory_order_relaxed);   // eye first
    slot.headSerial.store(headSerial, std::memory_order_relaxed);
    if (committedView)
        std::memcpy(slot.view, committedView, sizeof(slot.view));
    slot.viewValid.store(committedView != nullptr, std::memory_order_relaxed);
    slot.seq.store(seq, std::memory_order_release);   // then publish the slot
    g_aerRenderEye.store(eye, std::memory_order_release);
    g_aerStampArmed.store(true, std::memory_order_relaxed);
    g_aerStampSeq.store(seq, std::memory_order_release);
}

bool AerStampsArmed()
{
    return g_aerStampArmed.load(std::memory_order_acquire);
}

void SetSceneIsFlat(bool flat)
{
    g_sceneIsFlat.store(flat, std::memory_order_relaxed);
}

bool GetSceneIsFlat()
{
    return g_sceneIsFlat.load(std::memory_order_relaxed);
}

void SetFlatSceneMonoEnabled(bool enabled)
{
    g_flatSceneMono.store(enabled, std::memory_order_relaxed);
}

bool GetFlatSceneMonoEnabled()
{
    return g_flatSceneMono.load(std::memory_order_relaxed);
}

void ResetAerEyeSequence()
{
    g_aerRenderEye.store(0, std::memory_order_release);
    g_aerStampArmed.store(false, std::memory_order_release);
    g_aerStampSeq.store(0, std::memory_order_release);
    for (auto& slot : g_aerRing)
        slot.seq.store(0, std::memory_order_relaxed);
}

void SetAerConvergence(float tangentShift)
{
    if (std::isfinite(tangentShift) && tangentShift >= -0.25f && tangentShift <= 0.25f)
        g_aerConvergence.store(tangentShift, std::memory_order_release);
}

float GetAerConvergence()
{
    return g_aerConvergence.load(std::memory_order_acquire);
}

void SetFillFactor(float fill)
{
    if (fill > 0.35f && fill < 3.0f)
        g_fillFactor.store(fill, std::memory_order_relaxed);
}

float GetFillFactor()
{
    return g_fillFactor.load(std::memory_order_relaxed);
}

void SetGameProjection(float focalX, float focalY, float skew,
    float referenceFocalX, float referenceFocalY)
{
    g_gameFocalX.store(focalX, std::memory_order_relaxed);
    g_gameFocalY.store(focalY, std::memory_order_relaxed);
    g_gameSkew.store(skew, std::memory_order_relaxed);
    g_gameReferenceFocalX.store(referenceFocalX, std::memory_order_relaxed);
    g_gameReferenceFocalY.store(referenceFocalY, std::memory_order_relaxed);
}

bool GetHeadPose(float* yaw, float* pitch, float* roll, float* right,
    float* up, float* forward, std::uint64_t* serial)
{
    if (!yaw || !pitch || !roll || !right || !up || !forward || !serial)
        return false;
    AcquireSRWLockShared(&g_headPose.lock);
    const bool valid = g_headPose.valid;
    if (valid)
    {
        *yaw = g_headPose.yaw;
        *pitch = g_headPose.pitch;
        *roll = g_headPose.roll;
        *right = g_headPose.right;
        *up = g_headPose.up;
        *forward = g_headPose.forward;
        *serial = g_headPose.serial;
    }
    ReleaseSRWLockShared(&g_headPose.lock);
    return valid;
}

float GetNativeHeadTiming() { return g_nativeHeadTiming.load(std::memory_order_relaxed); }
void SetNativeHeadTiming(float offset)
{
    if (!std::isfinite(offset)) return;
    offset = std::clamp(offset, -1.0f, 1.0f);
    const auto previous = g_nativeHeadTiming.exchange(offset, std::memory_order_relaxed);
    if (previous != offset)
        XrLog("[HEADTIMING] offset=%+.2f samples; positive=older, negative=newer; orientation only", offset);
}

float GetHeadLookaheadFrames() { return g_headLookaheadFrames.load(std::memory_order_relaxed); }
void SetHeadLookaheadFrames(float frames)
{
    if (!std::isfinite(frames)) return;
    frames = frames < 0.0f ? -1.0f : std::min(frames, 4.0f);
    const auto previous = g_headLookaheadFrames.exchange(frames, std::memory_order_relaxed);
    if (previous != frames)
        XrLog("[LOOKAHEAD] pose prediction=%s%.2f frames (measured depth=%d)",
            frames < 0.0f ? "auto " : "", frames < 0.0f ? 0.0f : frames,
            g_headPipelineDepth.load(std::memory_order_relaxed));
}
int GetHeadPipelineDepth() { return g_headPipelineDepth.load(std::memory_order_relaxed); }
bool GetNativeTagCurrent() { return g_nativeTagCurrent.load(std::memory_order_relaxed); }
void SetNativeTagCurrent(bool current)
{
    if (g_nativeTagCurrent.exchange(current, std::memory_order_relaxed) != current)
        XrLog("[TAGMODE] native submit pose = %s", current ? "current upright (located for this present)" : "rendered (exact FIFO match)");
}
long long GetLastXrWaitMicros() { return g_lastXrWaitMicros.load(std::memory_order_relaxed); }
void SetSixDofCameraEnabled(bool enabled)
{
    g_sixDofCameraEnabled.store(enabled, std::memory_order_release);
}

void SetAfwEnabled(bool enabled)
{
    g_afwEnabled.store(enabled, std::memory_order_release);
}

bool GetAfwEnabled()
{
    return g_afwEnabled.load(std::memory_order_acquire);
}

void SetAfwFlip(bool flip)
{
    g_afwFlip.store(flip, std::memory_order_release);
}

bool GetAfwFlip()
{
    return g_afwFlip.load(std::memory_order_acquire);
}

void RecordHeadCameraPose(std::uint64_t serial, const float* committedView, int renderedEye)
{
    AcquireSRWLockExclusive(&g_headPose.lock);
    RenderedHeadPose committed{};
    const bool sampled = g_headPose.valid && g_headCameraHistory.Sample(serial, committed);
    committed.renderedEye = renderedEye;
    const bool recorded = sampled &&
        g_headCameraHistory.Commit(serial, committedView, GetTickCount64(), &committed);
    ReleaseSRWLockExclusive(&g_headPose.lock);
    if (!recorded) g_headCameraCommitMiss.fetch_add(1, std::memory_order_relaxed);
}

// [MONOTAG] Measured ([MONOCAM]): in Mono/AER the engine draws
// the camera committed one frame earlier ~75% of the time, a blend of two
// consecutive committed cameras ~15%, and one ~0.03 deg off the nearest the
// rest.  Stereo always draws a committed camera exactly.  The head pose of the
// camera actually drawn is recovered the same way: exact, blended between the
// two samples by the same fraction, or the nearest.
float CameraAngleDeg(const float* a, const float* b)
{
    // |A-B| of two rotations = 2*sqrt(2)*sin(angle/2); precise near zero,
    // where acos of the trace is not.
    double sum = 0.0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
        {
            const double d = static_cast<double>(a[r * 4 + c]) - b[r * 4 + c];
            sum += d * d;
        }
    const double s = std::min(1.0, std::sqrt(sum) / 2.8284271247461903);
    return static_cast<float>(2.0 * std::asin(s) * 57.29577951308232);
}
XrPosef BlendPose(const XrPosef& a, const XrPosef& b, float t)
{
    XrPosef out{};
    auto qb = b.orientation;
    if (a.orientation.x * qb.x + a.orientation.y * qb.y + a.orientation.z * qb.z + a.orientation.w * qb.w < 0.0f)
        qb = {-qb.x, -qb.y, -qb.z, -qb.w};
    XrQuaternionf q{a.orientation.x + (qb.x - a.orientation.x) * t, a.orientation.y + (qb.y - a.orientation.y) * t,
        a.orientation.z + (qb.z - a.orientation.z) * t, a.orientation.w + (qb.w - a.orientation.w) * t};
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    out.orientation = len > 1e-6f ? XrQuaternionf{q.x / len, q.y / len, q.z / len, q.w / len} : a.orientation;
    out.position = {a.position.x + (b.position.x - a.position.x) * t, a.position.y + (b.position.y - a.position.y) * t,
        a.position.z + (b.position.z - a.position.z) * t};
    return out;
}
bool FindDrawnCamera(const float* view, std::uint64_t now, RenderedHeadPose& result)
{
    constexpr int kDepth = 8;
    RenderedHeadPose poses[kDepth]{};
    float views[kDepth][16]{};
    int n = 0;
    while (n < kDepth && g_headCameraHistory.Recent(n, now, poses[n], views[n])) ++n;
    if (!n) return false;
    int nearest = 0;
    float nearestAngle = 1e9f;
    for (int k = 0; k < n; ++k)
    {
        const float a = CameraAngleDeg(view, views[k]);
        if (a < nearestAngle) { nearestAngle = a; nearest = k; }
    }
    // between two consecutive committed cameras (k newer, k+1 older)
    for (int k = 0; k + 1 < n; ++k)
    {
        const float ab = CameraAngleDeg(views[k], views[k + 1]);
        const float toOlder = CameraAngleDeg(view, views[k + 1]);
        const float toNewer = CameraAngleDeg(view, views[k]);
        if (ab > 0.01f && toOlder + toNewer - ab < 0.02f + 0.05f * ab)
        {
            const float t = std::clamp(toOlder / ab, 0.0f, 1.0f);
            result = poses[t < 0.5f ? k + 1 : k];
            for (int e = 0; e < 2; ++e)
                result.poses[e] = BlendPose(poses[k + 1].poses[e], poses[k].poses[e], t);
            g_monoTagBlend.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    if (nearestAngle < 0.25f)
    {
        result = poses[nearest];
        g_monoTagNearest.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void RecordNativeRenderCamera(const float* bankView, bool singleOutput)
{
    RenderedHeadPose pose{};
    AcquireSRWLockExclusive(&g_headPose.lock);
    const auto now = GetTickCount64();
    bool found = g_headPose.valid && g_headCameraHistory.Find(bankView, now, pose);
    if (singleOutput && bankView)
    {
        if (found) g_monoTagExact.fetch_add(1, std::memory_order_relaxed);
        else if (g_headPose.valid && IsSyncCameraView(bankView)) found = FindDrawnCamera(bankView, now, pose);
        if (!found) g_monoTagMiss.fetch_add(1, std::memory_order_relaxed);
    }
    if(found && bankView) std::memcpy(pose.markerView,bankView,sizeof(pose.markerView));
    // Every upload occupies one frame, including lookup failures. Do not skip
    // empty poses or replace an older queued image with a newer camera sample.
    if (!g_headFrameQueue.Push(pose, g_headPoseEpoch, now))
        g_headQueueDrops.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_headPose.lock);
    g_headCameraUploadThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    (found ? g_headCameraMatch : g_headCameraMiss).fetch_add(1, std::memory_order_relaxed);
}

void BeginHeadPosePresent()
{
    if (!g_headFrameLatch.Active())
    {
        HeadPoseAssociation::OrderedFrames<RenderedHeadPose>::Frame frame{};
        AcquireSRWLockExclusive(&g_headPose.lock);
        const auto now = GetTickCount64();
        const auto epoch = g_headPoseEpoch;
        const bool found = g_headFrameQueue.Pop(epoch, now, frame);
        g_headFrameQueueDepth = g_headFrameQueue.Size();
        ReleaseSRWLockExclusive(&g_headPose.lock);
        g_headFrameQueueSequence = found ? frame.sequence : 0;
        g_headFrameQueueAge = found ? now - frame.time : 0;
        g_headFrameLatch.Upload(found ? frame.pose : RenderedHeadPose{}, epoch);
    }
    g_headFrameLatch.Begin();
}
void EndHeadPosePresent() { g_headFrameLatch.End(); }

void MarkHeadPoseRendered(std::uint64_t serial)
{
    if (serial)
        g_renderedHeadPoseSerial.store(serial, std::memory_order_release);
}

void SetNativeImageHold(bool enabled)
{
    g_nativeImageHold.store(enabled, std::memory_order_release);
    XrLog("[IMAGEHOLD] requested=%d (native only, Pause toggles; not persisted)", enabled ? 1 : 0);
}
bool GetNativeImageHold()
{
    return g_nativeImageHold.load(std::memory_order_acquire);
}

void ResetHeadPoseCenter()
{
    InvalidateHeadPose(true);
}

void Shutdown()
{
    g_presenter.Shutdown();
}
}
