#include "desktop_present_policy.h"
#include "swapchain_inline_hooks.h"
#include "present_timing_capture.h"
#include "perf_census.h"
#include "steam_callback_limit.h"
#include "gameplay_log_filter.h"
#include "gpu_pose_trace.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <intrin.h>
#include <psapi.h>
#include <strsafe.h>
#include <tlhelp32.h>

#include "aer_control.h"
#include "world_lock_probe.h"
#include "aer_menu.h"
#include "display_spoof.h"
#include "openxr_presenter.h"
#include "present_profiler.h"
#include "fps_ab_probe.h"
#include "gbuffer_census.h"
#include "dlss_probe.h"
#include "dlss_upscaler.h"
#include "afw_synth.h"
#include "runtime_log.h"
#include "research_diagnostics.h"
#include "scene_census.h"
#include "ui_hook.h"
#include "stereo_task_fix.h"
#include "head_tracking_recovery.h"
#include "debug_register_update.h"
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace
{
constexpr std::uint32_t kRetailTimestamp = 0x5F85274E;
constexpr std::uint32_t kRetailImageSize = 0x0FECB000;

constexpr std::uintptr_t kDisplayGetterRva = 0x0ED75A80;
constexpr std::uintptr_t kCurrentOutputGetterRva = 0x0EDB7160;
constexpr std::uintptr_t kStrippedMultiOutputRva = 0x09207950;
constexpr std::uintptr_t kFullMultiOutputRva = 0x01074000;
constexpr std::uintptr_t kMatrixInverseRva = 0x000CDB00;
constexpr std::uintptr_t kUpdateRenderViewARva = 0x02D735B0;
constexpr std::array<std::uint8_t, 15> kUpdateRenderViewAPrefix = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
};
constexpr std::uintptr_t kEyeViewBuilderCallRva = 0x010740F9;
constexpr std::uintptr_t kEyeProjectionBuilderCallRva = 0x01074182;
constexpr std::uintptr_t kEyeViewBuilderTargetRva = 0x02D83FC0;
constexpr std::uintptr_t kEyeProjectionBuilderTargetRva = 0x02D84290;
constexpr std::uintptr_t kSepProbeProjectionCallRva = 0x01074263;
constexpr std::uintptr_t kSepProbeViewCallRva = 0x01074277;
constexpr std::uintptr_t kSepProbeProjectionTargetRva = 0x02BC0780;
constexpr std::uintptr_t kSepProbeViewTargetRva = 0x02BC0850;
constexpr std::uintptr_t kMatrixCacheWriterRva = 0x092140F0;
constexpr std::uintptr_t kMatrixCacheConstructorRva = 0x092131B0;
constexpr std::uintptr_t kPoseBufferRecorderRva = 0x0ED942C0;
constexpr std::uintptr_t kPassQueueRva = 0x0EABA5A0;
constexpr std::uintptr_t kUploadMatricesRva = 0x06768390;
constexpr std::uintptr_t kReplayCopyIatRva = 0x03067370;
constexpr std::uintptr_t kReplayCopyReturnRva = 0x02DB0DB6;
constexpr std::uintptr_t kActorLodBudgetRva = 0x00170560;
constexpr std::uintptr_t kActorLodManagerRunRva = 0x05FCF8A0;
constexpr std::uintptr_t kActorPointLodKernelRva = 0x05FD0920;
constexpr std::uintptr_t kActorBoundedLodKernelRva = 0x05FD0BC0;
constexpr std::uintptr_t kActorBoundedBudgetReturnRva = 0x05FD0CF4;
constexpr std::uintptr_t kTlsIndexRva = 0x05371AF0;
constexpr std::uintptr_t kFallbackRenderContextRva = 0x04F448B0;
constexpr std::uintptr_t kDisplayAreaObjectGlobalRva = 0x045DF038;
constexpr std::uintptr_t kReplayDispatchCallRva = 0x02DB290F;
constexpr std::uintptr_t kReplayMapCallRva = 0x02DB0D98;
constexpr std::uintptr_t kReplayDrawCallARva = 0x02DB1C1A;
constexpr std::uintptr_t kReplayDrawCallBRva = 0x02DB20E2;
constexpr std::uintptr_t kReplayDrawContinueRva = 0x02DB3925;

constexpr std::uintptr_t kPresenterBeginRva = 0x00480800;
constexpr std::uintptr_t kPresenterEndRva = 0x00482000;
constexpr std::uintptr_t kFullCameraJobBeginRva = kFullMultiOutputRva;
constexpr std::uintptr_t kFullCameraJobEndRva = kFullMultiOutputRva + 0x290;
constexpr std::array<std::uintptr_t, 7> kLifecycleOutputCallers = {
    0x0E8D4FC9, // DrawPhaseBegin: primary-eye reset gate
    0x0E8D5091, // DrawPhaseBegin: frame-time/update gate
    0x0E8D51BF, // DrawPhaseBegin: per-output preparation gate
    0x0E8D9926, // DrawPhaseEnd: last-eye resource release gate
    0x0E8DA05A, // DrawPhaseEnd: output-1 phase 0x3A release
    0x0E8DAC93, // post-End teardown: phases 0x50/0x51/0x55
    0x0E96D753, // renderer gate: output 1 must skip output-0 transient resource construction
};

// Passive renderer-pool census (Phase 2/3).  The two teardown functions
// FUN_14e8d98e0 / FUN_14e8dac40 release these DAT_144f294d8+offset slots; the
// last-eye-gated subset (marked) is the cross-eye shared lighting/history set.
// Sampling which slots are non-null across the mono->stereo->mono transition
// identifies the family that dies on stereo and is not reconstructed.
constexpr std::uintptr_t kPoolGlobalRva = 0x004F294D8; // &DAT_144f294d8 (retail base 0x140000000)

// [LMOBJ] light-manager object sampler.  DAT_144f29430 holds the deferred
// renderer context; the light manager lives at ctx+0xb826e0 (specular stage
// FUN_14ebf64f0 selects the light-SHAPE draw object from it: flag byte +0x168
// == 0 -> object pointer +0x160, else embedded object at +0xb0; the chosen
// object's vtable method +0x30 records the per-light shape draws that vanish
// under stereo).  Passive per-mode dump to find the field that collapses.
constexpr std::uintptr_t kDeferredCtxGlobalRva = 0x004F29430; // &DAT_144f29430
constexpr std::uintptr_t kLightManagerCtxOffset = 0xB826E0;
constexpr std::uintptr_t kLightShapeSelectorFlagOffset = 0x168;
constexpr std::uintptr_t kLightShapePointerOffset = 0x160;
constexpr std::uintptr_t kLightShapeEmbeddedOffset = 0xB0;

// [PROBEW] probe-weight producer FUN_14ebf3830 ("Light\Probe\
// LightProbeManagerBase.cpp(380): lightProbeWeightData_").  obj+0x60 = probe
// count, obj+0x64 = computed-empty latch: the body recomputes only when
// count != 0 || !valid, and sets valid = (count == 0).  If stereo empties the
// upstream count, the interior probe family dies exactly like the DRAW4
// signature.  Prologue = 12 bytes of pushes + SUB RSP,0x50 + 4-byte CMP ->
// steal 16 on an instruction boundary, all position-independent.
constexpr std::uintptr_t kProbeWeightsRva = 0x0EBF3830;
constexpr std::size_t kProbeWeightsStolen = 16;
constexpr std::array<std::uint16_t, 28> kPoolSlotOffsets = {
    0x378, 0x3F0, 0x418, 0x440, 0x4B8, 0x4E0, 0x508, 0x530, 0x558, 0x6C0,
    0x760, 0x788, 0x7B0, 0x7D8, 0x800, 0x828, 0x850, 0x8F0, 0x990, 0xAD0,
    0xAF8, 0xB20, 0xC88, 0xCB0, 0xCD8, 0xD00, 0xD28, 0xD50,
};
// Legend positions (0-based) of the last-eye-gated shared slots within the
// array above: 0x8F0=17, 0xC88=22, 0xCB0=23, 0xCD8=24, 0xD00=25, 0xD28=26, 0xD50=27.

constexpr std::ptrdiff_t kStereoEnableOffset = 0x244;
constexpr std::ptrdiff_t kStereoObjectOffset = 0x230;
constexpr std::ptrdiff_t kStereoBaselineOffset = 0x18;
constexpr std::ptrdiff_t kStereoScaleOffset = 0x1C;
constexpr std::ptrdiff_t kStereoSeparationOffset = 0x20;
constexpr std::ptrdiff_t kStereoConvergenceOffset = 0x24;
constexpr std::ptrdiff_t kCurrentOutputOffset = 0x344;
// +0x20 is an engine-clamped input whose stock value is already the maximum
// (1.0). Physical eye separation comes from +0x18 / +0x1C, so probe +0x18
// only and leave +0x20 at stock. Zeroing +0x20 produces invalid eye-0 matrices.
constexpr std::array<float, 3> kBaselineProbeMultipliers = {
    1.0f, 3.0f, 10.0f};
constexpr std::array<const char*, 3> kSepProbePresetNames = {
    "BASELINE_1X", "BASELINE_3X", "BASELINE_10X"};
constexpr std::size_t kSepProbeEventCount = 32;
constexpr bool kEnableOpenXr = true;
constexpr bool kInjectUploadMatrices = false;
constexpr bool kConstructEye1MatrixCache = false;
constexpr bool kForceBoundedFallbackInStereo = true;
// Arms the current stereo-hang probe ([POPGATE], DR2/DR3) at stereo-on.
// false = playable build.
constexpr bool kArmHangDiagnostics = false;

// [NATIVEONLY] Diagnostic: run FFXV's native stereo with none of the mod's
// gameplay-affecting patches (retail's own stripped camera job, no actor-LOD
// forcing), so the only change is the engine's stereo gate.  Stereo framing is
// degraded in this mode.  Interactive conversations freeze the same way with
// it on: the freeze belongs to the engine's own native stereo path.
constexpr bool kNativeStereoOnly = false;
// Runtime gate over the compile-time invariant above (F2).  The bounded-
// fallback forcing (+0x39 = 1 on every actor entry in stereo) is what restored
// NPC animation and collision, but it pins EVERY actor into the simplified
// work path.  Default ON = the protected behaviour; F2 turns it off.
std::atomic_bool g_boundedFallbackRuntimeEnabled{!kNativeStereoOnly};
// The forcing re-queues an actor every frame (the kernel enqueues whenever the
// requested/acknowledged bytes +0x39/+0x38 disagree).  In stereo that fires for
// ~36.5k entries per interval versus ~107 in mono - a flood of ambient NPCs
// that starves the downstream worker, so a conversation partner's animation
// step never lands.  Leaving stereo drains the backlog in one go, which is
// the symptom of an NPC completing a queued animation all at once.
// Near actors (the conversation partner) are exempted by distance; the ambient
// crowd the invariant exists for is still forced.  At hook time (before the
// kernel runs) the distance field reads 0 because entries are rebuilt each
// frame, so the exemption does not fire and the invariant behaves as before;
// [NEARACT] shows actor LOD is not what freezes the partner (level 0, balanced
// handshake, nothing queued).  Distance is stored INLINE in the work entry at
// +0x28 (the kernel does `*(float *)(param_2[2] + 8) = dist` where param_2 is a
// (*)[16] cursor, so param_2[2] is entry+0x20, not a pointer held there).
constexpr std::uintptr_t kActorEntryDistanceOffset = 0x28;
constexpr float kBoundedForceMinDistance = 10.0f;
std::atomic<std::uint64_t> g_boundedForceNearExempt{};
std::atomic<std::uint64_t> g_actorAckForced{};
// [NEARACT] Watch the closest actor's own state each second so mono and stereo
// can be diffed for the SAME NPC instead of reasoning about aggregates.
// [ACTOBJ] The frozen NPC's own object, not its LOD row.  The kernel reaches
// it via entry+0x20 (it reads *(int*)(obj+0x7c) there).  Snapshot a window of
// that object each second and report WHICH DWORDS CHANGED since the last
// sample.  Animating actor -> many fields churn; frozen actor -> few or none.
// The dwords that move in mono and stop in stereo are the stalled animation
// state, and their writer can then be found with a watchpoint.
constexpr std::size_t kActObjWindow = 0x180;
std::atomic<std::uintptr_t> g_actObjTracked{};
std::uint8_t g_actObjPrev[kActObjWindow]{};
std::atomic_bool g_actObjHavePrev{};
std::atomic<std::uint32_t> g_actObjLastLogMs{};
std::atomic<std::uint32_t> g_nearActLastLogMs{};
std::atomic<std::uint32_t> g_nearActBestDistanceBits{};
std::atomic<std::uint64_t> g_nearActBestState{};
constexpr bool kHoldBoundedFallbackThroughDownstream = true;
constexpr bool kClampNearbyBoundedAnimationLod = false;
constexpr float kBoundedAnimationLodDistance = 80.0f;
// [NEARLOD] The one measured NON-renderer difference between mono and stereo:
// mono keeps ~25% of bounded actor work entries at the finest tier (entry byte
// +0x38 == 0); in stereo that population is 0% - every actor is demoted, and
// the result-level histogram loses its top bin.  All 26 stereo-flag readers in
// the exe are renderer functions, so interaction is not stereo-gated directly.
// This forces near actors back to the finest tier in stereo (F11 toggles).
// [GATEWATCH] Reader census for the stereo gate byte (display+0x244) using
// hardware data breakpoints.  Static analysis finds 25 getter-callers + 1
// direct reader, all renderer code.  DR0 watches the byte on every thread; a
// vectored handler logs unique reader RVAs.  F12 arms/disarms.
// [CAMFIX] The full multi-output camera job (+0x1074000, which this proxy
// substitutes for retail's gutted +0x9207950) does TWO different things:
//   1. pushes per-eye view/projection into the DEFERRED RENDER CONTEXT
//      (+0x1074263 / +0x1074277) - this is what actually draws in stereo;
//   2. when the game has no per-output camera objects (retail: it does not),
//      writes those same eye matrices INTO THE SHARED CAMERA OBJECT and flags
//      it stereo (+0x4F5).  That shared camera is what GAMEPLAY reads to
//      project world positions to screen - interaction prompts, trigger
//      visibility, markers.  Feeding it an eye camera every frame while the
//      gate is on, and never while it is off, matches the exact repro:
//      standing still, F9 kills the prompts, F9 again brings them back.
// Retail's stripped job only ever CLEARS +0x4F5; substituting the full job
// means nothing clears it either.  So: NOP the four shared-camera writes and
// the flag store, keep the render-context pushes.  F6 toggles (default ON).
// Index 0..3 are the shared-camera matrix writes (the interaction fix), index 4
// is the +0x4F5 handshake store.  Retail's stripped job only CLEARS +0x4F5 and
// the full job SETS it; NOPing index 4 as well leaves the flag permanently 0,
// which stalls cutscenes, so index 4 has its own switch (F7).
constexpr std::array<std::uintptr_t, 5> kCamFixSites = {
    0x010741EE, 0x01074203, 0x01074226, 0x0107423E, 0x01074241};
constexpr std::array<std::size_t, 5> kCamFixSizes = {3, 3, 3, 3, 7};
constexpr std::size_t kCamFixFlagIndex = 4;
constexpr std::array<std::array<std::uint8_t, 3>, 4> kCamFixExpected = {{
    {0xFF, 0x53, 0x08}, {0xFF, 0x50, 0x10},
    {0xFF, 0x53, 0x08}, {0xFF, 0x50, 0x10}}};
std::array<std::array<std::uint8_t, 8>, 5> g_camFixOriginal{};
// [CAMWRAP] Wrapper around the full camera job: the job runs untouched and the
// shared camera's matrix block can be snapshotted/restored around it.  The
// renderer takes its matrices from the deferred-context pushes, a different
// object.  The proxy already redirects retail's stripped job here, so no
// trampoline is needed.
// setView (+0x9202EC0) writes the per-eye view block at camera+0x210 +
// eye*0x260 and a dword at camera+0x300 + eye*0x260: the eye blocks are
// +0x210 (eye 0) and +0x470 (eye 1).
constexpr std::uintptr_t kCamSnapshotBegin = 0x100;
constexpr std::uintptr_t kCamSnapshotEnd = 0x800;
// The setters' inner routine (FUN_1492140f0, reached from setView +0x9202EC0 and the setProj thunk
// +0x9202FB0) writes through a POINTER held in the block, into an external
// view cache -  entry stride 0x120, plus a current-index field and a flags
// array whose other entries get bit 3 cleared.  Gameplay projects with entry 0
// of that cache, which is exactly what the eye matrices overwrite.
// Blocks: proj at camera+0x110 + eye*0x260, view at camera+0x210 + eye*0x260.
constexpr std::array<std::uintptr_t, 4> kCamViewBlocks = {0x110, 0x210, 0x370, 0x470};
constexpr std::uintptr_t kBlockBufferPtr = 0x00;   // entries, 0x120 bytes each
constexpr std::uintptr_t kBlockFlagsPtr = 0x10;    // uint32 per entry
constexpr std::uintptr_t kBlockCount = 0x18;       // uint32
constexpr std::uintptr_t kBlockCurrentIndex = 0x20;// uint32
constexpr std::size_t kBlockEntryStride = 0x120;
constexpr std::uint32_t kBlockMaxEntries = 8;
constexpr std::uintptr_t kCamFlagByteA = 0x4F4;       // never restored: engine state
constexpr std::uintptr_t kCamFlagByteB = 0x4F5;
// The setters write outside the camera's own matrix block (into the view
// cache above), so restoring the block has no effect.  The wrapper only
// identifies the setters at runtime; the NOP fix is the default.
std::atomic_bool g_camWrapEnabled{false};
std::atomic_bool g_camVtableLogged{};
std::atomic<std::uint64_t> g_camWrapCalls{};
std::atomic<std::uint64_t> g_camWrapRestored{};
std::atomic_bool g_camWrapDiffLogged{};
using MultiOutputCameraJobFn = void(__fastcall*)(void*);
extern MultiOutputCameraJobFn g_realCameraJob;
extern "C" void __fastcall RetailCameraJobPolicy(void* camera);

std::array<std::atomic_bool, 5> g_camFixSiteApplied{};
// Interaction fix active.  The eye-0 setters do not own left-eye separation.
std::atomic_bool g_camFixEnabled{true};
// Handshake store: default = engine behaviour; the flag matters to something
// outside the renderer.  F7 toggles it.
std::atomic_bool g_camFlagStoreDisabled{false};
bool g_camFixCaptured{};
bool ApplyCamFixSite(std::size_t index, bool disable);
bool ApplyCamFix(bool enable);

// GATEWATCH target.  The readers of the stereo ENABLE byte (display+0x244) are
// all renderer/display code (34 of them).  The engine's multi-output test also
// reads a SECOND field:
//     if (display->stereoEnable != 0 || 1 < display->outputCount)   // +0x218
// Enabling stereo puts the whole engine in multi-output mode, and any system
// that branches on the output COUNT would change behaviour without ever
// touching the enable byte, so the watch covers the count (4 bytes).
constexpr std::ptrdiff_t kOutputCountOffset = 0x218;
std::atomic_bool g_gateWatchArmed{};
std::atomic<std::uint64_t> g_gateWatchHits{};
std::atomic<std::uint32_t> g_gateWatchArmedAtMs{};
std::atomic<std::uint32_t> g_gateWatchLastSweepMs{};
// No fixed window: the freeze happens several minutes into a multi-step scene,
// so the watch stays armed for as long as stereo is on and reports new
// readers as they appear.
constexpr std::uint32_t kGateWatchWindowMs = 0;
constexpr std::size_t kGateWatchSlots = 64;
std::array<std::atomic<std::uintptr_t>, kGateWatchSlots> g_gateWatchReaders{};
void* g_gateWatchHandler{};

// OFF by default: overwrites the engine's chosen LOD level with 0 every frame
// for every actor inside interaction range (stereo only), which can itself
// freeze a near NPC mid-dialogue.
std::atomic_bool g_nearLodRestoreEnabled{false};
constexpr float kNearLodRestoreDistance = 12.0f;   // interaction range is small
std::atomic<std::uint64_t> g_nearLodForced{};
std::atomic<std::uint64_t> g_nearLodSeen{};
constexpr bool kUseTrueOutputForDeferredManager = false;
constexpr std::uintptr_t kDeferredManagerOutputCallerRva = 0x0EBF55EA;
// Right-eye vehicle jitter: use the native per-eye bank at this selector.
// Atomic so it can be switched live without touching stereo, camera builders
// or the scheduler.
std::atomic_bool g_useTrueOutputForDisplayAreaBank{false};
constexpr std::uintptr_t kDisplayAreaBankOutputCallerRva = 0x067A1A70;
constexpr bool kUseMonoAmbientAoProof = false;

constexpr std::array<std::uint8_t, 16> kDisplayGetterPrefix = {
    0x48, 0x8B, 0x05, 0x91, 0x68, 0x1C, 0xF6, 0x48,
    0x8B, 0x80, 0x48, 0x03, 0x00, 0x00, 0xC3, 0x48,
};
constexpr std::array<std::uint8_t, 16> kCurrentOutputPrefix = {
    0x8B, 0x0D, 0x8A, 0xA9, 0x5B, 0xF6, 0x65, 0x48,
    0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0xBA,
};
constexpr std::array<std::uint8_t, 8> kStrippedMultiOutputPrefix = {
    0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0xCF,
};
constexpr std::array<std::uint8_t, 16> kFullMultiOutputPrefix = {
    0x40, 0x55, 0x57, 0x41, 0x57, 0x48, 0x8D, 0x6C,
    0x24, 0x90, 0x48, 0x81, 0xEC, 0x70, 0x01, 0x00,
};
constexpr std::array<std::uint8_t, 16> kMatrixCacheWriterPrefix = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
};
constexpr std::array<std::uint8_t, 16> kMatrixCacheConstructorPrefix = {
    0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE,
};
constexpr std::array<std::uint8_t, 16> kPoseBufferRecorderPrefix = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
};
constexpr std::array<std::uint8_t, 20> kPassQueuePrefix = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
    0x48, 0x83, 0xEC, 0x30,
};
constexpr std::array<std::uint8_t, 16> kUploadMatricesPrefix = {
    0x48, 0x89, 0xE0, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60,
};
constexpr std::array<std::uint8_t, 17> kActorLodBudgetPrefix = {
    0x40, 0x56, 0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x40,
    0x45, 0x0F, 0xB6, 0xD1, 0x0F, 0xB7, 0x74, 0x24,
    0x38,
};
constexpr std::array<std::uint8_t, 20> kActorLodManagerRunPrefix = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
    0x89, 0x7C, 0x24, 0x20,
};
constexpr std::array<std::uint8_t, 20> kActorPointLodKernelPrefix = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C,
    0x89, 0x74, 0x24, 0x20,
};
constexpr std::array<std::uint8_t, 15> kActorBoundedLodKernelPrefix = {
    0x48, 0x89, 0xE0, 0x48, 0x89, 0x58, 0x08, 0x48,
    0x89, 0x68, 0x10, 0x48, 0x89, 0x78, 0x18,
};
constexpr std::array<std::uint8_t, 6> kReplayDispatchCallPrefix = {
    0xFF, 0x90, 0x48, 0x01, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 8> kReplayMapCallPrefix = {
    0xFF, 0x50, 0x70, 0x48, 0x8B, 0x4C, 0x24, 0x50,
};
constexpr std::array<std::uint8_t, 8> kReplayDrawCallAPrefix = {
    0xFF, 0x50, 0x68, 0xE9, 0x03, 0x1D, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 8> kReplayDrawCallBPrefix = {
    0xFF, 0x50, 0x68, 0xE9, 0x3B, 0x18, 0x00, 0x00,
};

// Post-effect chain: the six callers of the per-output post-effect bank getter
// FUN_14ead4120.  The getter hard-returns null for output != 0 (retail strips
// output-1 banks), so under the output-0 compatibility alias these once-per-frame
// state machines run TWICE per stereo frame, double-stepping temporal/exposure
// state.  Eye-1 duplicates are skipped while [POSTSKIP] is enabled (F8 toggles).
constexpr std::size_t kPostChainCount = 6;
constexpr std::array<std::uintptr_t, kPostChainCount> kPostChainRvas = {
    0x067A2880, // bank-consuming state applier (indirectly called)
    0x0E8D4BB0, // bank consumer, 3-arg, called from +0xEBF64F0/+0xEBF9D30
    0x0E8D8590, // phase dispatcher param_5 stage A (largest, 3.7 KB)
    0x0E8D94F0, // phase dispatcher param_5 stage B
    0x0E8DA370, // phase dispatcher param_6 stage
    0x0E8DA8B0, // ForwardOnPostEffect (Test15/16 subject)
};
constexpr std::array<std::size_t, kPostChainCount> kPostChainStolen = {
    14, 15, 20, 17, 18, 14,
};
// Site 1 (+0xE8D4BB0) is a GETTER with out-pointer params (a crash at
// its caller +0xEBF67E2 proved skipping it leaves the caller reading
// uninitialized out-params).  It must always execute; counters only.
constexpr std::array<bool, kPostChainCount> kPostChainSkippable = {
    true, false, true, true, true, true,
};
constexpr std::array<std::uint8_t, 14> kPostChain0Prefix = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C,
    0x24, 0x20, 0x56, 0x57, 0x41, 0x54,
};
constexpr std::array<std::uint8_t, 15> kPostChain1Prefix = {
    0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0x81,
    0xF4, 0x21, 0xB8, 0x00, 0x4C, 0x89, 0xC3,
};
constexpr std::array<std::uint8_t, 20> kPostChain2Prefix = {
    0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24,
    0xB8, 0xFF, 0xFF, 0xFF,
};
constexpr std::array<std::uint8_t, 17> kPostChain3Prefix = {
    0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x58, 0x48,
    0x89, 0xCE, 0x4C, 0x8D, 0xB9, 0x50, 0x02, 0xB8,
    0x00,
};
constexpr std::array<std::uint8_t, 18> kPostChain4Prefix = {
    0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00,
    0x00, 0x00,
};
constexpr std::array<std::uint8_t, 14> kPostChain5Prefix = {
    0x48, 0x89, 0xE0, 0x56, 0x57, 0x41, 0x56, 0x48,
    0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00,
};

// Frame-end bank rotator FUN_142bbbe90 ("DrawManager.cpp(4456): ldrTex").
// Runs once per OUTPUT (proved by POSTSKIP counters on its param_6 sibling),
// but its frame-end work assumes one output per frame: the second (eye-1)
// invocation re-rotates the post-effect banks - it copies the now-null current
// bank (+0x378) into the previous-frame temporal slot (+0x530), re-runs the
// +0x468 <- +0x490 handoff after +0x490 was cleared, and allocates a second
// ldrTex.  Nulling the temporal feedback buffer every stereo frame is the
// lighting corruption.  Guard: snapshot the bank slots around the eye-1
// invocation and restore them, so its per-output commits still run but its
// frame-end writes are undone.  All release calls in that path are null-
// guarded and see nulls (eye-0 already rotated), so restoring cannot dangle.
constexpr std::uintptr_t kBankRotatorRva = 0x02BBBE90;
// Return-address RVA of the current-output getter call at +0x2BBBF1D inside
// the rotator; its EAX feeds the `!multiOutput || output == 0` handoff gate.
// (The earlier call at +0x2BBBEC4 is dead - RAX overwritten before use.)
constexpr std::uintptr_t kBankRotatorHandoffCallerRva = 0x02BBBF22;
constexpr std::array<std::uint8_t, 14> kBankRotatorPrefix = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
};
constexpr std::uintptr_t kBindingsPointerCtxOffset = 0xB820A8;
constexpr std::array<std::uint16_t, 7> kBankRotatorGuardSlots = {
    0x378, 0x3A0, 0x3C8, 0x468, 0x490, 0x508, 0x530,
};

// Pass-schedule census: FUN_14eaba5a0 registers named render passes
// ("ForwardOnPostEffect", "ForwardOnOpaque", ...).  Logging each unique
// (pass name, mode) pair across cleanmono/stereo-o0/stereo-o1/postmono lets
// one run diff the schedule and name the pass that vanishes under stereo.
constexpr std::uintptr_t kPassRegistrarRva = 0x0EABA5A0;
constexpr std::array<std::uint8_t, 15> kPassRegistrarPrefix = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
};

// Fiber-context output poisoning.  The current
// output index is PER-THREAD state (TLS block +0x344, default DAT_144f448b0).
// The job system saves/restores it as part of fiber switching; the restore
// path tail-jumps into setter FUN_14edb7540 at +0xED79434.  Fibers that ran
// output-1 stereo jobs keep output=1 in their saved contexts FOREVER after
// F9-off (nothing re-tags them in mono), and during stereo they run
// output-agnostic builder jobs (light lists, probe weights) branded output=1,
// whose per-output lookups hit retail's unconstructed output-1 data -> empty
// interior light lists.  The setter entry has a RIP-relative prologue, so the
// fiber restore JMP is redirected through a relay instead; the two remaining
// direct callers hardcode output 0 and need no hook.
constexpr std::uintptr_t kFiberOutputRestoreJmpRva = 0x0ED79434;
constexpr std::uintptr_t kSetCurrentOutputRva = 0x0EDB7540;
constexpr std::array<std::uint8_t, 5> kFiberOutputRestoreJmpPrefix = {
    0xE9, 0x07, 0xE1, 0x03, 0x00, // jmp +0xEDB7540
};
struct CodePatch
{
    void* address{};
    std::array<std::uint8_t, 32> original{};
    std::size_t size{};
    bool installed{};
};

HMODULE g_self{};
HMODULE g_realDxgi{};
std::uintptr_t g_exeBase{};
std::atomic_bool g_enginePatchesReady{false};
std::atomic_bool g_stereoEnabled{false};
std::atomic_bool g_aerEnabled{false};
// 32 mm per eye is a 64 mm virtual IPD: a neutral human-scale baseline.
std::atomic<float> g_aerHalfEyeMeters{0.0345f};   // [DEFAULTS] tested tuning
std::atomic_bool g_aerSwapEyes{true};   // [DEFAULTS]
std::atomic_uint64_t g_aerEyeOffsetApplied{};
std::atomic_bool g_sixDofEnabled{true};
std::atomic<float> g_sixDofPositionScale{1.0f};
// [SPLITSYM] Native stereo is asymmetric by engine design: the left pass
// renders from the plain gameplay camera and the camera job builds only the
// right pass (see [SWAYCAM]).  The
// right-eye seat adds the whole baseline, so "eye separation" only ever moved
// the right eye.  With this on, the gameplay camera itself is shifted by half
// the baseline the other way at the head-tracking write, so both eyes move:
// left = centre - b/2, right = (centre - b/2) + b = centre + b/2.
// Default OFF.  Measured: the head-tracking matrix is a
// camera-to-world transform (its translation row equals the builder source's
// world position, (-1978.6,110.4,-1522.2) vs (-1978.56,110.44,-1522.15)),
// while ApplyAerEyeOffset uses world-to-view math.  On this layout that math
// shifts along the WORLD X axis, not camera right, so the left half-offset is
// not trustworthy until [HEADLAYOUT] confirms the layout.  With it off, the
// right-eye seat - whose layout IS proven - carries the whole separation
// along true camera right: exact separation, midpoint b/2 left of centre.
std::atomic_bool g_nativeSymmetric{false};
std::atomic_uint64_t g_nativeLeftOffsetApplied{};

// [ISEQ] Watch on SequenceActionInteraction::execute.
//
// The interactive-conversation freeze in native stereo (Altissia summit and
// others): the interaction never starts, the sequence action that starts it
// polls forever, and flipping the engine to a single output releases it at
// once ([NATIVEONLY] proved it is the engine's own multi-output
// path).  The sequence tick calls each action's execute through vtable slot
// +0x130; this class's vtable sits at +0x36035E0.  DR3 breaks on the entry of
// that execute (no code patch, no ABI risk) so the mod knows, per frame,
// whether an interaction action is running, and snapshots the action object
// so the field it polls can be read off a diff.
constexpr std::uintptr_t kSeqActionInteractionVtableRva = 0x36035E0;
constexpr std::size_t kSeqExecuteSlotOffset = 0x130;
constexpr std::size_t kIseqSnapshotBytes = 0x1C0;
std::atomic_uintptr_t g_iseqExecuteAddress{};
std::atomic_bool g_iseqArmed{};
std::atomic_uint64_t g_iseqPolls{};
std::atomic_uint64_t g_iseqOtherVtableHits{};
std::atomic_uint64_t g_iseqLastPollPresent{};
std::atomic_uintptr_t g_iseqLastSelf{};
std::uint8_t g_iseqSnapshot[kIseqSnapshotBytes]{};
std::atomic_uint32_t g_iseqSnapshotSeq{};

// [AUTOMONO] While an interaction action is running in native stereo, drop
// the engine to a single output (exactly what the manual stereo-off toggle
// does, which is confirmed to release the freeze) and present that mono frame
// to both eyes with head tracking kept; restore two outputs once the action
// has stopped.  Conversations play mono, the world stays stereo.
// Conversation mode: 0 = keep stereo, 1 = mono (engine to one output,
// tracked), 2 = switch to AER for the conversation.  Measured:
// SequenceActionInteraction also runs in 3-7 present bursts every second or
// two during ordinary gameplay (ambient interaction points), so an episode
// only counts after kConversationMinPolling presents of continuous polling;
// reacting to every burst flipped the engine between one and two outputs
// constantly and flashed (some frames wireframe) through gameplay.
std::atomic_int g_conversationMode{1};
std::atomic_int g_conversationActive{};
// [SCRIPTCAM] Second trigger, measured in the summit scene: the
// engine's FOV setter is driven by camera channel 0 during play and switches
// to a different camera manager on channel 5 the moment the summit scene
// starts, right before the hang - while the interaction action never polled
// at all there.  A scripted camera channel held for a while marks a scene
// worth switching for, whatever sequence class runs it.
std::atomic_uint32_t g_fovLastChannel{};
std::atomic_uint64_t g_fovLastHitPresent{};
std::atomic_uint64_t g_fovZeroHits{};
std::atomic_uint64_t g_fovNonZeroHits{};
constexpr std::uint64_t kScriptedCameraMinPresents = 30;
// [SEQCENSUS] Which sequence action is the one that waits?  The sequence
// tick (+0x7BE0EE0, measured: actions[] +0x78, count +0x80,
// execute = vtable slot +0x130) calls every running action through ONE
// indirect call site.  DR2 breaks on that call instruction, where rcx is the
// action, so a single hardware breakpoint gives a per-tick census of every
// action class the engine executes.  During a hang the waiting action runs
// every tick; the moment stereo drops it completes and stops.  The class that
// was executing right before the manual stereo-off and stops executing right
// after it is the waiter - that is the diff this reports.
constexpr std::uintptr_t kSequenceTickRva = 0x7BE0EE0;
constexpr std::size_t kSequenceTickScanBytes = 0x800;
constexpr std::size_t kSeqCensusSlots = 128;
struct SeqCensusEntry
{
    std::atomic_uintptr_t vtableRva{};
    std::atomic_uint64_t hits{};
    std::atomic_uint64_t windowHits{};
    std::atomic_uint64_t firstPresent{};
    std::atomic_uint64_t lastPresent{};
    std::atomic_uint64_t sinceRelease{};
    std::atomic_bool activeAtRelease{};
    std::atomic_bool named{};
};
SeqCensusEntry g_seqCensus[kSeqCensusSlots]{};
std::atomic_uint32_t g_seqCensusReleaseStage{};
// Per-INSTANCE census, because classes are not enough: every scene class
// keeps executing once per tick for 300 presents
// after the release, because the node that follows the waiter is the same
// class (WaitTime after WaitTime, PlayMotion after PlayMotion).  The waiter
// is the instance that stops right after stereo-off; its successor is the
// instance first seen right after.
constexpr std::size_t kSeqInstSlots = 16384;
constexpr std::uint64_t kSeqInstStalePresents = 600;
struct SeqInstEntry
{
    std::atomic_uintptr_t self{};
    std::atomic_uintptr_t vtableRva{};
    std::atomic_uint64_t hits{};
    std::atomic_uint64_t firstPresent{};
    std::atomic_uint64_t lastPresent{};
    std::atomic_uint64_t sinceRelease{};
    std::atomic_bool activeAtRelease{};
};
SeqInstEntry g_seqInst[kSeqInstSlots]{};
std::atomic_uint64_t g_seqInstOverflow{};

// [TLPROBE] Measured: the one long-running instance that finished on
// its own schedule after the release was a SequenceActionTimeLineBlack
// (544 consecutive ticks during the hang, stopped 35 presents after
// stereo-off, never resumed).  Its execute (+0xA881BB0) wraps the base
// timeline execute (+0xA0F910), decompiled:
//   state +0x550 (2 = stopping); +0x57D set -> return running (paused);
//   +0x131 -> finish; dt = vcall+0x160 (* rate * speed +0x138);
//   +0x584 set && id +0x588 != -1 -> return running (WAIT ON ID);
//   +0x590 == 1 or +0x575 == 0 -> ready check +0xA10180, false -> running;
//   else time +0x578 += dt, seek, end test -> loop or finish.
// This snapshots every TimeLineBlack instance at execute entry so the log
// shows which branch holds it during the hang and what changes at release.
constexpr std::uintptr_t kTimeLineBlackVtableRva = 0x34FDA80;
constexpr std::size_t kTlSnapBytes = 0x700;
constexpr std::size_t kTlSlots = 16;
struct TimelineSlot
{
    std::atomic_uintptr_t self{};
    std::atomic_uint64_t hits{};
    std::atomic_uint64_t firstPresent{};
    std::atomic_uint64_t lastPresent{};
    std::atomic_uint32_t seq{};
    std::uint8_t snap[kTlSnapBytes]{};
    std::uint8_t releaseSnap[kTlSnapBytes]{};
    bool haveReleaseSnap{};
};
TimelineSlot g_timelines[kTlSlots]{};

// [AIWAIT] SequenceActionExecAIModeWait (+354BE80) and ExecAIModePlayMotion
// (+354B820) share the base execute +0xA155FB0 (decompiled):
//   +0xA157420(this, this+0x168) resolves the target actors into a vector at
//   +0x488 (count +0x490, 16-byte entries); count 0 -> finish.
//   status +0x480: 0 -> keep running (WAIT); 3 / 6 / 7 -> fire an output and
//   finish.  Nothing in execute writes +0x480: the AI-mode system sets it
//   from a completion callback.  The symptom "NPC never starts her next
//   animation" is exactly a PlayMotion whose completion never arrives, so
//   every instance of both classes is snapshotted and diffed at release.
constexpr std::uintptr_t kExecAIModeWaitVtableRva = 0x354BE80;
constexpr std::uintptr_t kExecAIModePlayMotionVtableRva = 0x354B820;
constexpr std::size_t kAiSnapBytes = 0x5C0;
constexpr std::size_t kAiSlots = 24;
struct AiWaitSlot
{
    std::atomic_uintptr_t self{};
    std::atomic_uintptr_t vtableRva{};
    std::atomic_uint64_t hits{};
    std::atomic_uint64_t firstPresent{};
    std::atomic_uint64_t lastPresent{};
    std::uint8_t snap[kAiSnapBytes]{};
    std::uint8_t releaseSnap[kAiSnapBytes]{};
    bool haveReleaseSnap{};
};
AiWaitSlot g_aiWaits[kAiSlots]{};

// [ACTORDIFF] Measured, with the symptom "the character
// does not sit down in stereo; stereo off and the animation plays": one
// SequenceActionExecAIModePlayMotion sat at status 0 for the whole scene
// and its object did not change by a single dword across the release.  The
// node is passive; the action is inside its target actor.  This snapshots
// the target actor of the longest-stuck PlayMotion and logs which dwords
// change from window to window (alive fields) and which fields START
// changing after stereo-off (the motion actually starting).
constexpr std::size_t kActorSnapBytes = 0x180; // the ActorHandle itself only
// [CHARDIFF] the character behind the handle.  Sampled every 10 presents
// while the PlayMotion waits; every dword that changes during the hang is
// marked volatile (animation clocks, positions, counters).  After the
// release, only dwords that were STABLE for the whole hang and then change
// are reported - the state that flips when the sit is allowed to start.
constexpr std::size_t kCharSnapBytes = 0x3000;
constexpr std::size_t kCharDwords = kCharSnapBytes / 4;
std::atomic_uintptr_t g_charTarget{};
std::uintptr_t g_charTracked = 0;
std::uint8_t g_charPrev[kCharSnapBytes]{};
std::uint8_t g_charRelease[kCharSnapBytes]{};
bool g_charVolatile[kCharDwords]{};
bool g_charHavePrev = false;
bool g_charHaveRelease = false;
std::uint32_t g_charSamples = 0;

// [CHARWATCH] Measured: during the hang only 26 of 3072 dwords of
// the ActorCharacter moved; five presents after stereo-off exactly these
// changed: qword +0x2E8 (object pointer swapped), qword +0x26F8 (object
// pointer swapped), and the dwords +0x7E4 and +0x2BF4, which held the SAME
// value through the hang and took new values together.  DR3 write-watches
// character+0x7E4 on every thread.  Each write records the writer's call
// stack (unwound from the trap context) and the value written.  Writes that
// happen only after stereo-off are the code path the hang is starved of;
// their callers carry the check that says no with two outputs.
constexpr std::size_t kCharWatchOffset = 0x7E4;
constexpr std::size_t kCharWatchEvents = 64;
constexpr std::size_t kCharWatchFrames = 12;
struct CharWatchEvent
{
    std::uint64_t present;
    std::uint64_t value;
    std::uint32_t frames;
    std::uint64_t stack[kCharWatchFrames];
    std::uint32_t thread;
};
CharWatchEvent g_charWatchEvents[kCharWatchEvents]{};
std::atomic_uint32_t g_charWatchWritten{};
std::atomic_uint32_t g_charWatchLogged{};
std::atomic_uint64_t g_charWatchHits{};
std::atomic_uint64_t g_charWatchDropped{};
std::atomic_uintptr_t g_charWatchAddress{};
std::atomic_bool g_charWatchArmed{};
DWORD WINAPI CharWatchArmThread(LPVOID parameter);
std::atomic_uintptr_t g_stuckActor{};
std::atomic_uintptr_t g_stuckActorNode{};
std::atomic_uint64_t g_stuckActorSince{};
std::uint8_t g_actorSnapPrev[kActorSnapBytes]{};
std::uint8_t g_actorSnapRelease[kActorSnapBytes]{};
bool g_actorSnapHavePrev = false;
bool g_actorSnapHaveRelease = false;
std::uintptr_t g_actorSnapTarget = 0;
std::atomic_uintptr_t g_seqCensusCallSite{};
std::atomic_bool g_seqCensusArmed{};
std::atomic_uint64_t g_seqCensusHits{};
std::atomic_uint64_t g_seqCensusOverflow{};
std::atomic_uint64_t g_seqCensusReleasePresent{};
std::atomic_bool g_seqCensusReleaseReported{};
constexpr std::uint64_t kConversationMinPolling = 45;
constexpr std::uint64_t kConversationEndGap = 30;
std::atomic_bool g_stereoSuspended{};
std::atomic_uint64_t g_autoMonoSuspendPresent{};
std::atomic_uint64_t g_autoMonoRestorePresent{};
std::atomic_uint32_t g_autoMonoEpisodes{};
std::atomic_uint64_t g_aerGateForced{};
// [SPLITGEO] Measured camera positions, so the separation that actually
// reaches the two passes is read from the log instead of assumed: the left
// eye as committed at the gameplay-camera write, and the right pass's source
// position before and after the right-eye seat adds the baseline.
std::atomic<float> g_splitLeftEye[3]{};       // committed head matrix, raw row 3
std::atomic<float> g_splitRightSource[3]{};   // builder camera transform, row 3
std::atomic<float> g_splitRightFinal[3]{};    // same, after the right-eye seat
std::atomic<float> g_headGameRow3[3]{};       // game's own head matrix, raw row 3
std::atomic<float> g_headGameViewEye[3]{};    // same matrix read as world-to-view
std::atomic<float> g_splitLeftViewEye[3]{};   // committed head matrix read as world-to-view
std::atomic<float> g_splitLeftAxes[9]{};      // committed head matrix columns 0..2
std::atomic_uint64_t g_splitLeftSamples{};
std::atomic_uint64_t g_splitRightSamples{};
std::atomic_bool g_customFirstPersonEnabled{false};
std::atomic_bool g_liveFovOverrideEnabled{false};
std::atomic<float> g_liveFovDegrees{110.0f};
// [IVFOV] Render-bound projection override.  Writes to the camera-block
// source+0x40 lanes have no visual effect, so this override acts on the
// constant-buffer uploads seen by the hooked contexts, immediate and deferred.
// The perspective projections travel in 128- and 512-byte WRITE_DISCARD-mapped
// CBs, so identification is per-upload by payload fingerprint alone - no
// size constant, no cached resource identity, no learned baseline.
constexpr float kIvFovReferenceDegrees = 100.0f;
std::atomic_uint64_t g_ivFovPatched{};
std::atomic_uint64_t g_ivFovVpPatched{};
std::atomic_uint64_t g_ivFovInvPatched{};
std::atomic_uint64_t g_ivFovSkippedPartial{};
std::atomic_uint64_t g_ivFovNonDiscardMaps{};
struct IvFovSizeCensusCell
{
    std::atomic_uint32_t size{};
    std::atomic_uint64_t count{};
    std::atomic_uint64_t patched{};
    std::atomic_bool dumped{};
};
std::array<IvFovSizeCensusCell, 24> g_ivFovSizeCensus{};
std::atomic_uint32_t g_ivFovMainCamLogs{};
std::atomic_uint32_t g_ivFovVscb1Logs{};
std::atomic_uint64_t g_ivFovCopy1Calls{};
std::atomic_uint64_t g_ivFovLastDumpPresent{};
std::atomic_uint64_t g_ivFovCopyCalls{};
std::atomic_uint32_t g_ivFovCopySmallLogs{};
std::atomic_uint32_t g_ivFovCopyBigLogs{};
std::atomic_uint64_t g_fovScaleApplies{};

// [CAMPARAM] Camera parameter table - the FOV lever, found by decompiling the
// engine.
//
// Black.Sequence.Action.Camera.Init.SequenceActionInitCameraParameter is the
// action the working title_envsys BCPT_FOV asset runs.  Its execute override
// (+0xA382020) ends with:
//
//     value = *(u32*)(this + 0x1B4);              // cameraParamValue_
//     if (guard && *(u32*)(this + 0x1B0) < 0x44F) // cameraParamType_ (BCPT_*)
//         table[type] = value;
//
// so the engine keeps one flat u32 array of camera parameters indexed by the
// BCPT_ enum.  Setting the FOV slot by asset visibly works, which makes this
// table the effective FOV input; camera +0x300/+0x304/+0x308, source+0x40
// and the constant buffers all sit downstream of it.
//
// The BCPT_FOV ordinal is NOT hardcoded: the enum member names do not exist in
// the packed executable, and the assets store the name rather than the index.
// It is identified by observation instead - the watcher below diffs the table
// and reports the slot that the game itself moves to the installed asset's
// degree value.
constexpr std::uintptr_t kCamParamExecuteRva = 0xA382020;
constexpr std::uintptr_t kCamParamTableRva = 0x4F01F30;
constexpr std::uintptr_t kCamParamGuardRva = 0x4CEDEE8;
constexpr std::uint32_t kCamParamCount = 0x44F;
// Prologue of the execute override; proves this executable matches the
// analysis the table address came from.
constexpr std::uint8_t kCamParamSignature[] = {
    0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x44,
    0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,
};
// The table stores FLOAT bit patterns, not integers
// (slot 1 = 0x425C0000 = 55.0, slot 7 = 0x41400000 = 48.0 - the engine's
// default vertical FOV).  Detection and driving are therefore float-typed.
//
// The engine populates the whole table in one burst at
// startup.  A flat per-change log budget is spent entirely on that burst, so
// bulk fills are absorbed as a new baseline and only incremental changes -
// the asset's FOV write, and whatever first-person/cutscenes do - are logged.
constexpr std::uint32_t kCamParamBulkChangeThreshold = 32;
constexpr float kCamParamFovLow = 40.0f;
constexpr float kCamParamFovHigh = 145.0f;
std::atomic_bool g_camParamValidated{};
std::atomic_bool g_camParamSnapshotTaken{};
std::atomic_int g_camParamFovIndex{-1};
std::atomic<float> g_camParamReferenceDegrees{100.0f};
std::atomic_uint32_t g_camParamChangeLogs{};
std::atomic_uint32_t g_camParamCandidateLogs{};
std::atomic_uint32_t g_camParamBulkFills{};
std::atomic_uint64_t g_camParamDrives{};
std::atomic_uint32_t g_camParamLastWritten{};

// Both runtime lanes below have no visible effect (camera +0x300 writes are
// not picked up; CPU-side constant-buffer patching cannot reach a
// GPU-composed projection), and the constant-buffer patching flickers the
// auxiliary views, so both are gated off.
constexpr bool kEnableIvFovPatchLane = false;
constexpr bool kEnableCameraScalarLane = false;
std::array<std::uint32_t, kCamParamCount> g_camParamSnapshot{};
// The engine's own value per slot, tracked separately from the live table so
// scaling never compounds on top of a value this mod already wrote.
std::array<std::uint32_t, kCamParamCount> g_camParamBaseline{};
// What this mod last wrote per slot, so the watcher can tell engine activity
// from its own writes.
std::array<std::uint32_t, kCamParamCount> g_camParamOurWrite{};
// Slot 7 is proven to drive normal gameplay, but first person and cutscenes
// never write the table (log-proven across a full session) and still flicker -
// consistent with those cameras alternating between this global and their own
// preset FOV.  All-modes mode makes every camera FOV parameter agree by
// scaling each slot from its own engine baseline by the same ratio, which
// preserves the relative framing the presets were authored with.
std::atomic_bool g_camParamAllModes{false};
std::atomic_uint64_t g_camParamAllModeWrites{};
std::atomic_uint32_t g_camParamAllModeSlots{};

// [FOVSINK] Find the camera object's real FOV field by using slot 7 as an
// oracle.
//
// Slot 7 provably changes gameplay FOV, so whatever field the renderer finally
// consumes must track it.  Scanning the camera object at the camera-job seat
// for a field that follows the slider - in degrees, radians, or either focal
// form - identifies that sink without assuming offsets.
//
// First person and cutscenes never touch the camera parameter table
// (scaling all 28 slots has no effect on them), so
// they must write this same sink from the TimeLine track-item path
// (Black.System.TimeLine.TrackItem.Camera.InGameCameraFovTrackItem and
// friends).  Owning the sink owns every mode at once.
constexpr std::uint32_t kFovSinkScanBytes = 0x800;
constexpr std::uint32_t kFovSinkSlots = kFovSinkScanBytes / 4;
enum class FovSinkKind : std::uint8_t
{
    None = 0,
    Degrees,
    Radians,
    CotHalf,
    TanHalf,
};
struct FovSinkCell
{
    std::atomic_uint32_t matches{};
    std::atomic<float> lastTarget{};
    std::atomic_uint8_t kind{};
};
std::array<FovSinkCell, kFovSinkSlots> g_fovSinkCells{};
std::atomic_int g_fovSinkOffset{-1};
std::atomic_uint8_t g_fovSinkKind{};
std::atomic_uint32_t g_fovSinkLogs{};
std::atomic_uint64_t g_fovSinkWrites{};
std::atomic_uint64_t g_fovSinkProbes{};
std::atomic_uintptr_t g_fovSinkCamera{};

// [FOVCHAN] The engine's real FOV system, decompiled offline from the
// slot-7 reader chain (FUN_1410de9f0 -> setter FUN_1491923d0):
//
//   - the camera object holds NINE prioritized FOV channels, 0x30 bytes
//     each, at camera+0x2C68;
//   - every mode sets its OWN channel (gameplay = channel 0 fed by
//     parameter-table slot 7; first person and cutscene tracks use higher
//     channels with a priority gate at channel+0x2C94-style slots);
//   - the engine scans the per-channel active flags and stores a pointer to
//     the WINNING channel at camera+0x2E18; the consumed value is that
//     channel's float at +0x14 (the setter itself reads the current value
//     back from exactly there).
//
// This is why a single global value does not reach FP/cutscenes: they win by
// priority on their own channel.  The override therefore writes the value
// slot of ALL channels, before and after the camera job, so whichever
// channel is active carries the requested lens.  Values are radians
// (factory converts: slot7 * pi * (1/180)).
constexpr std::ptrdiff_t kFovChanBase = 0x2C68;
constexpr std::ptrdiff_t kFovChanStride = 0x30;
constexpr std::uint32_t kFovChanCount = 9;
constexpr std::ptrdiff_t kFovChanValue = 0x14;
constexpr std::ptrdiff_t kFovChanActivePtr = 0x2E18;
std::atomic_uint64_t g_fovChanWrites{};
std::atomic_uint32_t g_fovChanLogs{};

// [FOVSET] Hardware-breakpoint hook on the engine's FOV setter
// FUN_1491923d0 (+0x91923D0), found offline via the slot-7 reader chain.
// Entry code: value in XMM1 (radians), manager in RCX, channel in R8D.
// Every per-mode FOV write - gameplay, first person, cutscene tracks,
// all 17 callers - passes through here, so overriding XMM1 at entry owns
// the lens for every mode, upstream of channels, priorities and
// interpolators.  Uses DR1 (DR0 belongs to head tracking) with the same
// VEH resume pattern.  The first hit also captures the TRUE manager
// object pointer, which the camera-job object is not (FOVCHAN wrote
// nothing: the channel table is not on the job camera).
constexpr std::uintptr_t kFovSetterRva = 0x91923D0;
std::atomic_bool g_fovSetArmed{};
std::atomic_uint64_t g_fovSetHits{};
std::atomic_uintptr_t g_fovSetManager{};
std::atomic_uint32_t g_fovSetLogs{};
// Pin both lens endpoints while retaining native transition timing/completion.
std::atomic_bool g_fovPinTransitionStart{true};
std::atomic_uint64_t g_fovTransitionStartPinned{};


// The camera source stages its projection at +0x40.  Writing it is proven
// dead (two hooks, thousands of writes, zero visual change), but READING it
// truthfully reports the lens the game is asking for in each camera mode.
constexpr std::ptrdiff_t kDynamicProjectionOffset = 0x40;

// [MODEPROBE] Pure measurement, no writes.
//
// Measures the lens each camera mode renders, rather than inferring it from
// the screen.  First person and cutscenes can look
// "orthographic" in the headset while head tracking still works, which has two
// completely different causes:
//
//   1. the mode genuinely renders a narrow (telephoto) FOV, which compresses
//      perspective and looks flat, or
//   2. both eyes render from the same position, so there is no stereo at all -
//      which also looks flat, and would leave head tracking working perfectly.
//
// This probe measures both, per mode, with no dependence on the FOV override:
// the projection the game itself stages at source+0x40 (dead to WRITE, but a
// truthful read of the FOV the game intends), and the actual world position
// written for each eye, so the real stereo separation can be compared against
// the configured IPD.
struct ModeProbeState
{
    std::atomic<float> eyePosition[2][3]{};
    std::atomic_uint32_t eyeSeen{};
    std::atomic<float> lastFovVertical{};
    std::atomic_uint64_t lastLogPresent{};
    std::atomic_uint32_t logs{};
};
ModeProbeState g_modeProbe{};

// [LENS] Measure the REAL rendered lens.
//
// source+0x40 is NOT a projection: its p00 goes negative while p11 stays
// near 1.0, which are rotation-matrix components.
//
// The real main-camera projection is the one the constant-buffer census
// already caught: p00/p11 = 1.2634/2.2460, i.e. a 48-degree vertical lens.
// The auxiliary view family sits near p11 0.95 and is excluded by band.
// This scan is read-only, always on, and independent of the FOV override, so
// it reports the lens each camera mode actually renders.
constexpr float kLensP11Low = 1.05f;
constexpr float kLensP11High = 4.0f;
std::atomic<float> g_lensLastVertical{};
std::atomic_uint64_t g_lensLastLogPresent{};
std::atomic_uint32_t g_lensLogs{};
std::atomic_uint64_t g_lensSamples{};
// [LENSFEED] The OpenXR presenter claims a FIXED frustum while the game's
// rendered lens more than doubles between modes (gameplay 48 deg vertical,
// cutscenes and first person 82-87 deg, animating).  An image whose claimed
// FOV does not match its rendered FOV has its disparity scaled wrong, which
// reads as compressed depth - flat - however correct the eye separation is.
//
// The presenter supports a reference/current focal ratio fed from [LENS],
// which reads the real main-camera projection out of the constant buffer
// (source+0x40 is not a projection and would squash the image vertically).
//
// The reference is the measured gameplay baseline, so at that lens the ratio
// is exactly 1; only a genuinely different lens moves the claim.  Fed from the 512-byte buffer
// only, which is the one that consistently carries the main camera.
constexpr float kLensBaselineP00 = 1.2634f;
constexpr float kLensBaselineP11 = 2.2460f;
std::atomic_bool g_lensFeedEnabled{true};
std::atomic_uint64_t g_lensFeedWrites{};
// No acceptance/hysteresis logic: the first in-band sample of a session is
// the TITLE SCREEN's 35-degree cinematic camera, and a latched value would
// then block later legitimate mode changes.  One deterministic rule with NO
// memory: each frame, claim the widest main-band lens rendered that frame.
std::atomic_uint64_t g_lensFramePresent{};
std::atomic_uint32_t g_lensFrameP11Bits{};
std::atomic_uint32_t g_lensFrameP00Bits{};

// [LENSHIST] WIDTH does not discriminate the raster lens: an auxiliary camera
// sharing these CBs sits at a constant ~83 degrees, so with the lens pinned
// at 87 a width rule alternates between the two (a third-person size
// flicker).
//
// The stable difference is not width, it is TRAFFIC: the raster projection is
// uploaded once per scene pass (many times a frame), an auxiliary or probe
// camera once or twice.  So bucket this frame's main-band lenses by value and
// claim the most frequently uploaded one, ties going to the wider.  Still one
// deterministic rule per frame with no memory across frames, so nothing can
// latch.
constexpr std::uint32_t kLensBuckets = 8;
std::atomic_uint32_t g_lensBucketP11Bits[kLensBuckets]{};
std::atomic_uint32_t g_lensBucketP00Bits[kLensBuckets]{};
std::atomic_uint32_t g_lensBucketHits[kLensBuckets]{};
std::atomic_uint32_t g_lensBucketOffset[kLensBuckets]{};
std::atomic_uint32_t g_lensBucketUsed{};
std::atomic_uint64_t g_lensHistLogPresent{};

void ResetLensBuckets()
{
    for (std::uint32_t i = 0; i < kLensBuckets; ++i)
        g_lensBucketHits[i].store(0, std::memory_order_relaxed);
    g_lensBucketUsed.store(0, std::memory_order_release);
}

// Fold one main-band sample into this frame's histogram.  Values within 1
// percent are the same lens; the engine rebuilds the matrix per pass, so the
// low bits are not stable.
void AddLensSample(float p11, float p00, std::uint32_t offset)
{
    const auto used = g_lensBucketUsed.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < used && i < kLensBuckets; ++i)
    {
        float stored = 0.0f;
        const auto bits = g_lensBucketP11Bits[i].load(std::memory_order_relaxed);
        std::memcpy(&stored, &bits, sizeof(stored));
        if (stored > 0.0f && std::fabs(stored - p11) <= stored * 0.01f)
        {
            g_lensBucketHits[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    auto slot = used;
    while (slot < kLensBuckets &&
        !g_lensBucketUsed.compare_exchange_weak(slot, slot + 1,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
    }
    if (slot >= kLensBuckets)
        return;
    std::uint32_t p11Bits = 0;
    std::uint32_t p00Bits = 0;
    std::memcpy(&p11Bits, &p11, sizeof(p11Bits));
    std::memcpy(&p00Bits, &p00, sizeof(p00Bits));
    g_lensBucketP11Bits[slot].store(p11Bits, std::memory_order_relaxed);
    g_lensBucketP00Bits[slot].store(p00Bits, std::memory_order_relaxed);
    g_lensBucketOffset[slot].store(offset, std::memory_order_relaxed);
    g_lensBucketHits[slot].store(1, std::memory_order_release);
}

// [MODEPROBE] The head hook fires for MANY view sources (camera positions
// jumped hundreds of units between consecutive samples), so eye separation is
// only meaningful when both eyes are compared for the SAME source.
struct EyePairCell
{
    std::atomic_uintptr_t source{};
    std::atomic<float> position[2][3]{};
    std::atomic_uint64_t present[2]{};
    std::atomic_uint32_t seen{};
};
std::array<EyePairCell, 16> g_eyePairs{};
std::atomic_uint32_t g_headSourceLogs{};

// [EYEHOLD] Alternate-eye rendering shows each eye a DIFFERENT MOMENT.
//
// Measured: with one view source and a configured IPD of 0.0284 m,
// the distance between the committed left-eye and right-eye camera positions
// was 0.077, 0.15, 0.19, 0.36, 0.71, 1.33, 1.76, 2.43 m - three to eighty
// times the eye separation.  The camera's own motion between the two frames
// dominates the disparity, so the eyes do not form a stereo pair: they form
// two views of a moving camera.  When the camera is still the same measurement
// lands on 0.0293 vs 0.0261 expected, which is exactly why gameplay reads as
// solid depth while cutscenes and first person - where the camera is always
// moving - read as flat and flickery.
//
// The lens is not the cause: gameplay renders 48.0 deg vertical and those
// modes render WIDER (80-87 deg), not narrower.
//
// Fix: hold the game camera's own transform across an eye pair.  The first
// eye of a pair captures it; the second eye renders from the captured
// transform instead of the game's newer one.  Both eyes then differ only by
// the IPD offset applied afterwards, which is what stereo fusion requires.
// The cost is that camera motion advances once per pair rather than once per
// frame - the tradeoff alternate-eye rendering already makes.
// Default ON.  The view census shows callsPerFrame min=1 max=1: exactly ONE
// main camera per frame.  Measured camera motion is 0.11-2.63 world units per
// frame against an 0.088 IPD - 2x to 30x the eye separation - so without the
// hold the eyes see different moments.  The [EYEHOLD] fields on the
// MODEPROBE line report it directly.
std::atomic_bool g_eyeHoldEnabled{true};
// [AERFRESH] Off: the hold replays the first eye's HEAD pose on the second
// eye too, so AER head tracking advances once per PAIR.  On: every eye is
// tagged with the exact head sample it was rendered with ([AERTAG]), so the
// second eye can take a fresh sample and the compositor still lines the pair
// up.  The game's own camera stays held either way (that is what keeps the
// pair's world consistent).  Insert menu toggle.
std::atomic_bool g_aerFreshHeadPose{false};
alignas(16) float g_eyeHoldMatrix[16]{};
std::atomic_uint64_t g_eyeHoldPresent{};
std::atomic_bool g_eyeHoldValid{};
// [AERSYNC] pair self-check, written by the camera build, read by the update.
std::atomic_uint64_t g_aerPairGapUm{}, g_aerPairMoveUm{}, g_aerPairSamples{};
std::atomic_uint64_t g_eyeHoldApplied{};
std::atomic_uint64_t g_eyeHoldCaptured{};
std::atomic_uint64_t g_eyeHoldStale{};
// Did the hold actually substitute the captured matrix on this frame?
// Logged beside eyeSep so the two can never be reasoned about apart.
std::atomic_bool g_eyeHoldAppliedThisFrame{};
// The capture is taken from the RAW matrix before the head pose is applied.
// Holding only the matrix would leave both eyes with a shared base transform
// but a DIFFERENT live head pose stacked on top, putting per-frame difference
// straight back into the pair, so the pose is captured and replayed with it.
struct EyeHoldPose
{
    float yaw{};
    float pitch{};
    float roll{};
    float right{};
    float up{};
    float forward{};
    std::uint64_t serial{};
};
EyeHoldPose g_eyeHoldPose{};
SRWLOCK g_eyeHoldLock = SRWLOCK_INIT;

// [VIEWCEN] Which of the many views passing through the camera source is the
// MAIN camera?
//
// ONE scratch buffer (a single src pointer per session) carries many views per
// frame - camera positions jump hundreds of units between consecutive samples
// on the SAME pointer - so a transform applied there also lands on shadow,
// reflection and sub-camera views, not only the player's.
//
// Views arrive in a stable order within a frame, so each invocation is keyed by its
// index within the present.  The main camera is the slot whose position moves
// CONTINUOUSLY - small, non-zero, consistent frame-to-frame deltas that track
// player input.  Shadow cascades snap or sit still; reflection views mirror.
// Read-only: the census never modifies the matrix.
constexpr std::uint32_t kViewCenSlots = 32;
struct ViewCensusSlot
{
    std::atomic_uint64_t samples{};
    std::atomic<float> lastPosition[3]{};
    std::atomic<float> deltaSum{};
    std::atomic<float> deltaMax{};
    std::atomic<float> lastForward[3]{};
    std::atomic_bool seen{};
};
std::array<ViewCensusSlot, kViewCenSlots> g_viewCensus{};
std::atomic_uint64_t g_viewCenPresent{};
std::atomic_uint32_t g_viewCenIndex{};
std::atomic_uint32_t g_viewCenPerFrameMin{0xFFFFFFFFu};
std::atomic_uint32_t g_viewCenPerFrameMax{};
std::atomic_uint64_t g_viewCenFrames{};
std::atomic_uint64_t g_viewCenLastDump{};
// Most recent value seen at each call index, published by the hook and read
// by the present path.  The hook runs inside a vectored exception handler, so
// it must never log or take a lock: it only stores.
struct ViewCensusLive
{
    std::atomic<float> position[3]{};
    std::atomic<float> forward[3]{};
    std::atomic_uint64_t present{};
};
std::array<ViewCensusLive, kViewCenSlots> g_viewCensusLive{};
std::atomic_uint32_t g_viewCenLastFrameCalls{};
std::atomic<float> g_customFirstPersonRight{};
std::atomic<float> g_customFirstPersonUp{};
std::atomic<float> g_customFirstPersonForward{};
std::atomic_uint64_t g_sixDofPositionApplied{};

bool VrCameraEnabled()
{
    return g_stereoEnabled.load(std::memory_order_acquire) ||
        g_aerEnabled.load(std::memory_order_acquire);
}

std::atomic_uint64_t g_policyTrueCamera{};
std::atomic_uint64_t g_policyTruePresenter{};
std::atomic_uint64_t g_policyTrueLifecycle0{};
std::atomic_uint64_t g_policyTrueLifecycle1{};
std::atomic_uint64_t g_policyAliased{};
std::atomic_uint64_t g_policyTrueDeferred{};
std::atomic_uint64_t g_policyTrueDisplayAreaBank{};
std::atomic_uint64_t g_matrixCachePassed{};
std::atomic_uint64_t g_matrixCacheSkipped{};
std::atomic_uint64_t g_matrixCacheConstructed{};
std::atomic_uint32_t g_matrixCacheConstructionState{};
std::atomic_uintptr_t g_uploadMatricesManager{};
std::atomic_uint32_t g_uploadMatricesArmed{};
std::atomic_uint64_t g_uploadMatricesOutput0{};
std::atomic_uint64_t g_uploadMatricesOutput1{};
std::atomic_uint64_t g_uploadMatricesInjected{};
std::atomic_uint64_t g_uploadMatricesInjectionFailed{};
std::atomic_uint64_t g_actorLodBudgetMonoCalls{};
std::atomic_uint64_t g_actorLodBudgetStereoCalls{};
std::array<std::atomic_uint64_t, 2> g_actorLodBudgetOutputCalls{};
std::array<std::atomic_uint64_t, 8> g_actorLodBudgetGroupCalls{};
std::array<std::atomic_uint64_t, 8> g_actorLodBudgetInputLevelCalls{};
std::array<std::atomic_uint64_t, 8> g_actorLodBudgetResultLevelCalls{};
std::array<std::array<std::atomic_uint64_t, 8>, 2> g_actorLodDistanceBins{};
std::atomic_uint64_t g_actorLodViewLastLogMs{};
std::array<std::atomic_uint64_t, 2> g_actorPointFallbackZero{};
std::array<std::atomic_uint64_t, 2> g_actorPointFallbackSet{};
std::array<std::atomic_uint64_t, 2> g_actorPointStateZero{};
std::array<std::atomic_uint64_t, 2> g_actorPointStateSet{};
std::array<std::atomic_uint64_t, 2> g_actorBoundedFallbackZero{};
std::array<std::atomic_uint64_t, 2> g_actorBoundedFallbackSet{};
std::array<std::atomic_uint64_t, 2> g_actorBoundedState34Zero{};
std::array<std::atomic_uint64_t, 2> g_actorBoundedState34Set{};
std::array<std::atomic_uint64_t, 2> g_actorBoundedState36Zero{};
std::array<std::atomic_uint64_t, 2> g_actorBoundedState36Set{};
std::atomic_uint64_t g_actorBoundedFallbackForced{};
std::atomic_uint64_t g_actorBoundedAnimationLodForced{};
std::array<std::atomic_uint64_t, 2> g_actorBoundedByteSampleSequence{};
std::array<std::array<std::atomic_uint64_t, 64>, 2> g_actorBoundedByteZero{};
std::array<std::array<std::atomic_uint64_t, 64>, 2> g_actorBoundedByteOne{};
std::array<std::array<std::atomic_uint64_t, 64>, 2> g_actorBoundedByteOther{};
struct ActorLodCallerSample
{
    std::atomic_uintptr_t rva{};
    std::array<std::array<std::atomic_uint64_t, 8>, 2> levels{};
};
std::array<ActorLodCallerSample, 16> g_actorLodCallers{};
std::atomic_uint64_t g_presentCount{};
std::atomic_bool g_f8Down{};
std::atomic_bool g_f9Down{};
std::atomic_int g_vrEnableKey{VK_F9};   // [KEYBIND] rebindable via the Insert menu
// [MONOKEY] failsafe: switches Stereo or AER straight to Mono (a conversation
// that hangs in stereo).  Rebindable in the Stereo section of the menu.
std::atomic_int g_monoKey{VK_END};
std::atomic_bool g_monoKeyDown{};
std::atomic_bool g_f10Down{};
std::atomic_bool g_numpad0Down{};
std::atomic_bool g_numpad1Down{};
std::atomic_bool g_numpad2Down{};
std::atomic_bool g_numpad3Down{};
std::atomic_bool g_afwEnabled{false};
std::atomic_bool g_afwFlip{false};
std::atomic_int g_sepProbePreset{-1};
std::atomic_uint64_t g_sepProbeDumpAtPresent{};
std::atomic_uint64_t g_commandListsFinished{};
std::atomic_uint64_t g_commandListsFinishFailed{};
std::atomic_uint64_t g_commandListsExecuted{};
std::atomic_uint64_t g_contextVtablesHooked{};
std::atomic_bool g_commandProbeInstalled{};
std::atomic_bool g_resourceProbeInstalled{};
std::atomic_uint64_t g_buffersCreated{};
std::atomic_uint64_t g_buffersDestroyed{};
std::atomic_uint64_t g_bufferBytesCreated{};
std::atomic_uint64_t g_bufferBytesDestroyed{};
std::atomic_uint64_t g_texturesCreated{};
std::atomic_uint64_t g_texturesDestroyed{};
std::atomic_uint64_t g_textureBytesCreated{};
std::atomic_uint64_t g_textureBytesDestroyed{};
std::atomic_uint32_t g_stereoTextureSamples{};
std::atomic_uint32_t g_depthStackSamples{};
std::atomic_uint32_t g_depthDestroyStackSamples{};
std::atomic_int64_t g_lastTelemetryTicks{};
std::atomic_uint64_t g_lastBankTelemetryMs{};
std::uint64_t g_lastPrivateBytes{};
std::uint64_t g_lastFinished{};
std::uint64_t g_lastExecuted{};

CodePatch g_outputGetterPatch{};
CodePatch g_multiOutputPatch{};
CodePatch g_matrixCachePatch{};
CodePatch g_poseBufferRecorderPatch{};
CodePatch g_passQueuePatch{};
CodePatch g_uploadMatricesPatch{};
using SepProbeUploadFn = void(__fastcall*)(void*, const float*, const float*, std::uint32_t);
struct SepProbeMatrixSlot
{
    SRWLOCK lock = SRWLOCK_INIT;
    float matrix[16]{};
    std::uint64_t writes{};
    std::uint32_t threadId{};
    float separation{};
    float baseline{};
};
std::array<std::array<SepProbeMatrixSlot, 2>, 2> g_sepProbeMatrices{};
using EyeMatrixBuilderFn = float* (__fastcall*)(
    void*, float*, const float*, float, std::uint32_t);
CodePatch g_eyeViewBuilderCallPatch{};
CodePatch g_eyeProjectionBuilderCallPatch{};
CodePatch g_sepProbeProjectionCallPatch{};
CodePatch g_sepProbeViewCallPatch{};

void* g_eyeViewBuilderRelay{};
void* g_eyeProjectionBuilderRelay{};
void* g_sepProbeProjectionRelay{};
void* g_sepProbeViewRelay{};
EyeMatrixBuilderFn g_realEyeViewBuilder{};
EyeMatrixBuilderFn g_realEyeProjectionBuilder{};
SepProbeUploadFn g_realSepProbeProjectionUpload{};
SepProbeUploadFn g_realSepProbeViewUpload{};
using MatrixInverseFn = void(__fastcall*)(const float*, float*);
MatrixInverseFn g_matrixInverse{};
using UpdateRenderViewFn = void(__fastcall*)(void*, float*, float*, std::uint32_t);
CodePatch g_updateRenderViewPatch{};
UpdateRenderViewFn g_realUpdateRenderView{};
std::atomic_uint64_t g_headTrackCalls{};
std::atomic_uint64_t g_headTrackApplied{};
std::atomic_uint64_t g_headTrackRejected{};
std::atomic_uint64_t g_headTrackHookAttempts{};

enum class DynamicHeadPhase : std::uint32_t
{
    Off,
    CaptureSource,
    WatchWriter,
    Active,
};
struct DynamicHeadWriterSlot
{
    std::atomic_uintptr_t rip{};
    std::atomic_uint32_t hits{};
};
constexpr std::size_t kDynamicHeadWriterSlots = 32;
std::array<DynamicHeadWriterSlot, kDynamicHeadWriterSlots> g_dynamicHeadWriters{};
std::atomic<DynamicHeadPhase> g_dynamicHeadPhase{DynamicHeadPhase::Off};
std::atomic_uintptr_t g_dynamicHeadCaptureSite{};
std::atomic_uintptr_t g_dynamicHeadSource{};
std::atomic_uintptr_t g_dynamicHeadWriter{};
std::atomic_uint32_t g_dynamicHeadCaptureHits{};
std::atomic_uint32_t g_dynamicHeadWriterWatchHits{};
std::atomic_bool g_dynamicHeadWorkerRunning{};
std::atomic_bool g_dynamicHeadStopRequested{};
// [MODETRACK] Stereo and AER use the SAME writer (+0x921424E), so a mode
// switch never needs a new one.  Switching modes leaves ~270 ms with no VR
// mode on, during which the worker can shut down; the menu order plus
// FinishDynamicHeadWorker() keep tracking alive across it.  The rescan flag
// below is only the head-tracking key's "find it again" action.
std::atomic_bool g_dynamicHeadRediscoverRequested{};
bool DynamicHeadTrackingRunning();
void RediscoverDynamicHeadWriter();
void StartDynamicHeadTracking();
void SetAer(bool enable);
// [MODETRACK2] A crash (ffxv_s.exe+0xEBC2514, null render target
// in a temporal pass): switching with AER turned on BEFORE native stereo was
// off.  The engine only ever saw the old order - one mode fully off, then the
// other on - so that order is kept, and this flag carries head tracking over
// the short gap instead: the worker keeps running while it is set, while the
// camera writer itself (VrCameraEnabled) still does nothing in the gap.
std::atomic_bool g_vrModeSwitching{};
bool VrTrackingWanted()
{
    return VrCameraEnabled() || g_vrModeSwitching.load(std::memory_order_acquire);
}
// [VRMODESAVE] 1 = Stereo, 2 = AER: the mode the head tracking key starts
// from Mono.  Saved by the Insert menu ([Display] VrMode).
std::atomic_int g_preferredVrMode{1};
std::atomic_uint32_t g_dynamicHeadLastSweepMs{};
// Passive Chapter 12 observer. DR0 keeps the confirmed tracking writer;
// DR2 records the renderer's returned camera without changing it.
std::atomic_uintptr_t g_cameraObserverSite{};
std::atomic_uintptr_t g_cameraObserverLastSource{};
std::atomic_uint64_t g_cameraObserverHits{};
std::atomic_uint64_t g_cameraObserverMismatches{};
SRWLOCK g_headRecoveryLock = SRWLOCK_INIT;
HeadTrackingRecovery::Health g_headRecoveryHealth{};
std::atomic_uint64_t g_headRecoveryAttempts{};

void* g_dynamicHeadVeh{};
std::atomic_uint64_t g_nativeEyePairsBuilt{};
std::atomic_uint64_t g_symmetricEyeCommits{};
std::atomic_uint64_t g_symmetricEyeFallbacks{};
struct SepProbeEvent
{
    std::atomic_bool ready{};
    std::uint64_t generation{};
    std::uint64_t jobSequence{};
    std::uint64_t present{};
    void* camera{};
    std::uint32_t threadId{};
    int output{};
    std::uint32_t type{};
    float separation{};
    float baseline{};
    float matrix[16]{};
};
std::array<SepProbeEvent, kSepProbeEventCount> g_sepProbeEvents{};
std::atomic_uint64_t g_sepProbeEventGeneration{};
std::atomic_uint32_t g_sepProbeEventNext{};
std::atomic_bool g_sepProbeEventRecording{};
std::atomic_uint64_t g_sepProbeJobSequence{};
thread_local void* g_sepProbeActiveCamera{};
thread_local std::uint64_t g_sepProbeActiveJobSequence{};
struct NativeEyeBuildState
{
    std::uint64_t jobSequence{};
    bool ready[2]{};
    alignas(16) float matrices[2][2][16]{};
};
thread_local NativeEyeBuildState g_nativeEyeBuild{};
std::atomic_uint64_t g_leftCameraDeferred{};
std::atomic_uint64_t g_leftCameraReplayed{};
std::atomic_uint64_t g_leftCameraReplayFailed{};
CodePatch g_replayCopyIatPatch{};
CodePatch g_actorLodBudgetPatch{};
CodePatch g_actorLodManagerRunPatch{};
CodePatch g_actorPointLodKernelPatch{};
CodePatch g_actorBoundedLodKernelPatch{};
CodePatch g_replayDispatchCallPatch{};
CodePatch g_replayMapCallPatch{};
CodePatch g_replayDrawCallAPatch{};
CodePatch g_replayDrawCallBPatch{};
std::array<CodePatch, kPostChainCount> g_postChainPatches{};
CodePatch g_bankRotatorPatch{};
// Default OFF: restoring pre-call slot values dangled a live bank pointer that
// ForwardOnPostEffect consumes later in the same eye-1 phase (crash
// at +0xE8DA982, first guarded rotation).  The engine re-creates the current
// bank BETWEEN the two rotator invocations; any guard must preserve the
// rotator's forward-feeding outputs.  Do not re-enable without the full
// pointer-flow map from the disassembly.
std::atomic_bool g_bankRotGuardEnabled{false};
std::atomic_uint64_t g_bankRotEye0{};
std::atomic_uint64_t g_bankRotGuarded{};
std::atomic_uint64_t g_bankRotGuardFailed{};
// Rotator handoff-gate true-output: correct engine behaviour, kept ON (it is
// not the cause of the lighting symptom).
std::atomic_bool g_bankRotGateTrue{true};
std::atomic_uint64_t g_policyTrueBankRotGate{};
CodePatch g_passRegistrarPatch{};
CodePatch g_fiberOutputPatch{};
void* g_fiberOutputRelay{};
CodePatch g_probeWeightsPatch{};
using ProbeWeightsFn = void(__fastcall*)(void*, void*, void*, std::uint8_t);
ProbeWeightsFn g_realProbeWeights{};
std::atomic<std::uint64_t> g_probewCalls[3]{};
std::atomic<std::uint64_t> g_probewLastState{0xFFFFFFFFFFFFFFFFull};
std::atomic<std::uint32_t> g_probewLastLogMs{};
std::atomic_bool g_everStereo{};
// Gate registry for [PROBEG]: the producer hook records the two distinct
// producer objects and the ctx it is called with; the Present-side sampler
// reads the caller gates from them (caller1: ctx+0x1b20 bit2 + obj+0x2D8/9;
// caller2: obj+0x6D0/1; force global DAT_1444f1f30).
std::atomic<std::uintptr_t> g_probewObjs[2]{};
std::atomic<std::uintptr_t> g_probewCtx{};
constexpr std::uintptr_t kProbeForceGlobalRva = 0x044F1F30; // DAT_1444f1f30
constexpr std::uintptr_t kProbeViewFlagsOffset = 0x1B20;
// [PROBEF] force-pulse: PROBEG proved producer B is killed by viewFlags
// (ctx+0x1B20) bit2 going 0 at F9 and latching.  DAT_1444f1f30 is the
// engine's own override in caller1 (+0xEAE27A0): nonzero -> producer B runs
// regardless of the mask (caller resets it to 0 each run).  Pulsing it every
// present while stereo/post-stereo re-runs B; visual heal = owner confirmed.
std::atomic_bool g_probeForceEnabled{false};
std::atomic<std::uint64_t> g_probeForcePulses{};
// [QMASK] FUN_14679e050 (+0x679E050) applies a per-output settings
// blob when multi-output is active (sole caller +0x67A1901 in FUN_1467a1890,
// blob = base + output*0x13 + 0x40).  blob[1] != 0 = the native SBS
// "reduced quality" preset: kills the probe-relight master byte
// (deferredCtx+0xB827D0), clears viewFlags(ctx+0x1B20) bits 0+2 (bit2 gates
// probe producer B - the interior probe family), and ORs six disable bits
// into the 64-bit feature mask at ctx+0x6E0.  Mono never runs this path, so
// the reduction latches after F9-off.  Fix = pass the engine a copy of the
// blob with byte 1 cleared: full lighting in stereo, nothing to un-latch.
// Prologue = 15 position-independent bytes (2x MOV [RSP+..], PUSH RDI,
// SUB RSP,0x20) -> trampoline-safe.
constexpr std::uintptr_t kQualityMaskApplyRva = 0x0679E050;
constexpr std::size_t kQualityMaskStolen = 15;
constexpr std::size_t kQualityBlobSize = 0x13;
CodePatch g_qualityMaskPatch{};

// [UPDISP] The sampling diff's biggest signal: FUN_145BC3870 accounts for 46
// samples in the working mono window and ZERO in the frozen stereo window.  It
// is a per-frame update dispatcher - it walks the list at manager+0xBBF8 and
// calls virtual slot +0x140 on every node.  Sampling absence is suggestive,
// not proof, so count the calls directly and capture its arguments; if the
// count really is zero in stereo, this is the work that stops happening and
// the arguments show what to drive it with.
constexpr std::uintptr_t kUpdateDispatchRva = 0x05BC3870;
constexpr std::size_t kUpdateDispatchStolen = 15;
CodePatch g_updateDispatchPatch{};
using UpdateDispatchFn = void(__fastcall*)(void*, void*);
UpdateDispatchFn g_realUpdateDispatch{};
std::atomic<std::uint64_t> g_updateDispatchCalls[2]{};
std::atomic<std::uintptr_t> g_updateDispatchArg0{};
std::atomic<std::uintptr_t> g_updateDispatchArg1{};
extern "C" void __fastcall RetailUpdateDispatchPolicy(void* manager, void* context);
using QualityMaskFn = void(__fastcall*)(const std::uint8_t*, void*);
QualityMaskFn g_realQualityMask{};
std::atomic_bool g_qualityKeepEnabled{true};
std::atomic<std::uint64_t> g_qualityKeepHits{};
std::atomic<std::uint64_t> g_qualityPassthrough{};
// [VFIX] corrective write on the SINGLETON view-flags dword.  The clearer
// computes it as *(base+0x4F29430) + 0xB80250 + 0x1B20 (verified == the ctx
// the probe producer receives), so the address is stable at runtime.  The
// +0x679E050 clearer is NOT the F9-time writer (blob[1]==0 every time, flags
// still 0x1F->0x1A), so bits 0+2 are re-ORed every present during/after
// stereo and counted: a couple of corrections per session means a one-shot
// writer, a count climbing per frame means a per-frame writer.
constexpr std::uintptr_t kViewCtxOffset = 0xB80250;
constexpr std::uintptr_t kViewFlagsOffset = 0x1B20;   // dword, bits 0+2
constexpr std::uintptr_t kProbeRelightMasterOffset = 0xB827D0; // byte
constexpr std::uintptr_t kFeatureMask64Offset = 0x6E0; // qword in view ctx
std::atomic_bool g_viewFlagsFixEnabled{true};
// [EYEMERGE] REMOVED: merging per-byte max/min across "blobs"
// took ASCII from a bogus restore pointer ("cuda_...", "try ") and pushed it
// into the rendering state -> white terrain.  See [BLOBCHECK] in the restore.

std::atomic<std::uint64_t> g_viewFlagsCorrections{};
// Mid-frame corrections (in the probe-producer hook = light-build phase,
// after the per-frame reducer, before producer B's caller checks the gate;
// Present-time correction proved too LATE - probes flashed on alternate
// frames).  Mono baselines: viewFlags bits0+2 set, relight byte 1, mask6e0
// bit31 clear.
std::atomic<std::uint64_t> g_midFlagsCorrections{};
std::atomic<std::uint64_t> g_relightCorrections{};
std::atomic<std::uint64_t> g_maskBitCorrections{};
std::atomic_bool g_stereoFiberClampEnabled{true};
std::atomic<DWORD> g_phaseThreadId{};
std::atomic_uint64_t g_outsetValue0{};
std::atomic_uint64_t g_outsetValue1{};
std::atomic_uint64_t g_outsetOther{};
std::atomic_uint64_t g_outsetHealed{};
std::atomic_uint64_t g_outsetClamped{};
std::atomic_bool g_stereoEverEnabled{false};
// seen-set for (nameHash, mode) pairs; open addressing, write-once slots
constexpr std::size_t kPassCensusSlots = 512;
std::array<std::atomic_uint64_t, kPassCensusSlots> g_passCensusSeen{};
// Default OFF: eye-1 NEEDS these stages to run (skipping them unbinds
// per-output state; skin shaders render white on the right eye).
std::atomic_bool g_postChainSkipEnabled{false};
std::array<std::atomic_uint64_t, kPostChainCount> g_postChainExecuted{};
std::array<std::atomic_uint64_t, kPostChainCount> g_postChainSkipped{};
void* g_replayDispatchRelay{};
void* g_replayMapRelay{};
void* g_replayDrawRelayA{};
void* g_replayDrawRelayB{};
std::uint8_t* g_stereoEnableAddress{};
float* g_stereoSeparationAddress{};
float* g_stereoBaselineAddress{};
float* g_stereoScaleAddress{};
float* g_stereoConvergenceAddress{};
std::uint8_t g_savedStereoEnable{};
float g_savedStereoSeparation{};
float g_savedStereoBaseline{};
float g_savedStereoScale{};
float g_savedStereoConvergence{};
bool g_savedStereoData{};

SRWLOCK g_logLock = SRWLOCK_INIT;
HANDLE g_logHandle = INVALID_HANDLE_VALUE;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FinishCommandListFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
    D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
    const D3D11_BOX*, const void*, UINT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
    UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using VSSetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
    ID3D11Buffer* const*);
using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using CopyResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*,
    ID3D11Resource*);
using CopySubresourceRegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using CopySubresourceRegion1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*,
    UINT);
using UpdateSubresource1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
using VSSetConstantBuffers1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

struct ContextVtableHook
{
    std::atomic<void**> table{};
    std::atomic<void**> alias{};   // [VTALIAS] a copy of `table` swapped in by someone else
    ExecuteCommandListFn execute{};
    FinishCommandListFn finish{};
    MapFn map{};
    UnmapFn unmap{};
    UpdateSubresourceFn update{};
    DispatchFn dispatch{};
    VSSetConstantBuffersFn vsSetConstantBuffers{};
    DrawFn draw{};
    DrawIndexedFn drawIndexed{};
    DrawIndexedInstancedFn drawIndexedInstanced{};
    DrawInstancedFn drawInstanced{};
    CopyResourceFn copyResource{};
    CopySubresourceRegionFn copySubresourceRegion{};
    CopySubresourceRegion1Fn copySubresourceRegion1{};
    UpdateSubresource1Fn updateSubresource1{};
    VSSetConstantBuffers1Fn vsSetConstantBuffers1{};
};
constexpr std::size_t kContextVtableHookCount = 8;
std::array<ContextVtableHook, kContextVtableHookCount> g_contextVtableHooks{};
SRWLOCK g_contextHookLock = SRWLOCK_INIT;

struct PoseBufferSample
{
    std::atomic_uintptr_t resource{};
    std::atomic_uintptr_t mappedData{};
    std::atomic_uint32_t lastMapType{};
    std::atomic_uintptr_t callerRva{};
    std::atomic_uintptr_t recordCallerRva{};
    std::atomic_uintptr_t replaySource{};
    std::atomic_uintptr_t sourceWriterRva{};
    std::atomic_uintptr_t producerMapCallerRva{};
    std::atomic_uintptr_t producerUnmapCallerRva{};
    std::atomic_uintptr_t updateCallerRva{};
    std::atomic_uintptr_t updateSource{};
    std::atomic_uint64_t lastHash{};
    std::atomic_uint32_t writes{};
    std::atomic_uint32_t changes{};
    std::atomic_uint32_t stackLoggedModes{};
    std::atomic_uint64_t vsBindCalls[3]{};
    std::atomic_uint64_t vsHashChanges[3]{};
    std::atomic_uint64_t vsLastHash[3]{};
    std::atomic_uint64_t vsEqualO1AtO0{};
    std::atomic_uint32_t vsSlotsMask[3]{};
    std::atomic_uintptr_t vsLastShader[3]{};
    std::atomic_uint32_t ivProjSeen{};
    std::atomic_uint32_t ivPatchedFlag{};
    std::atomic_uint64_t ivBinds{};
    std::atomic_uint32_t ivLastSlot{};
    std::atomic_uint32_t ivBindOffset{};
    std::uint32_t byteWidth{};
    std::uint32_t bindFlags{};
    std::uint32_t monoReference{};
};
constexpr std::size_t kPoseBufferSampleCount = 8192;
std::array<PoseBufferSample, kPoseBufferSampleCount> g_poseBuffers{};
std::atomic_uint64_t g_poseBuffersObserved{};
thread_local PoseBufferSample* g_mappedPoseSample{};
std::array<std::atomic_uint32_t, 3> g_viewCbBoundLogs{};
std::atomic_uint32_t g_viewCbStackLogs{};
std::atomic_uint64_t g_vsCbBindCalls[3]{};
VSSetConstantBuffersFn g_realEngineVsSetConstantBuffers{};
constexpr std::size_t kViewCbPayloadBytes = 992;
struct ViewCbPayloadSnapshot
{
    SRWLOCK lock = SRWLOCK_INIT;
    std::array<std::uint8_t, kViewCbPayloadBytes> bytes{};
    std::atomic_uint64_t generation{};
    std::uint64_t present{};
    std::uint64_t hash{};
    std::uint64_t jobSequence{};
    std::uint32_t threadId{};
    std::uint32_t trueOutput{};
    std::uint64_t sourceGeneration{};
};
ViewCbPayloadSnapshot g_viewCbPayloadLatest{};
std::array<ViewCbPayloadSnapshot, 3> g_viewCbPayloads{};
// Write-time buckets: filled in HookUnmap on the producer thread, keyed by the
// producer's own true output.  The bind-time buckets above can lag by whole
// frames (generation=11492/2/4 in a dump), so layout math uses
// these.
std::array<ViewCbPayloadSnapshot, 3> g_viewCbPayloadsAtWrite{};
std::atomic_uintptr_t g_viewCbPayloadResource{};
std::atomic_uint64_t g_viewCbPayloadDumpAtPresent{};
std::atomic_bool g_viewCbPayloadDumped{};
std::atomic_uint32_t g_viewCbPayloadStackModes{};
std::atomic_bool g_viewCbPayloadMapStackLogged{};

struct NativeEyePairSnapshot
{
    SRWLOCK lock = SRWLOCK_INIT;
    alignas(16) float matrices[2][2][16]{};
    std::uint64_t jobSequence{};
    std::uint64_t present{};
    std::uint32_t threadId{};
    std::uint32_t readyMask{};
    float separation{};
    float baseline{};
};
NativeEyePairSnapshot g_nativeEyePairLatest{};

// Output-0 skew probe.  Proven layout: payload 0x000 = V*P
// (row-vector), 0x040 = inverse, 0x080 = V.  Stock skewed payloads carry
// projection skew P[2][0] = baseline/scale; the left view's payloads carry
// the mono projection with no skew, which is the eye-separation asymmetry
// that shows.  Keying on the TLS-output tag and a single selected resource
// patches shadow-cascade payloads (flicker) and misses the left view's
// writes, so the probe recognizes the
// main camera by content on EVERY 992-byte buffer: a perspective payload
// satisfies A[i][3] == -V[i][2] (shadow cascades are orthographic and fail),
// skewed perspective writes update the reference (skew + camera position),
// and skewless perspective writes at the same camera position get the
// opposite skew injected in place before unmap.  Measure and patch are
// exclusive branches of one observation, so no feedback loop is possible.
// Probe modes cycled by F8: PROBE injects the opposite skew into skewless
// main-camera writes; SWAY oscillates the skew term of EVERY main-camera
// write (skewed and skewless) so each half's consumption of CPU-written
// payloads is directly visible on screen; OFF leaves payloads untouched.
// SWAYCAM is the live lane: oscillate the camera-source
// transform the eye builders read, transiently, to prove which render paths
// derive from it.  The buffer-lane modes (PROBE/SWAY992/SWAYOBJ) are closed
// but kept reachable for reference.  F8 from the default goes straight to
// OFF.
constexpr std::uint32_t kO0SkewModeSwayCam = 0;
constexpr std::uint32_t kO0SkewModeOff = 1;
constexpr std::uint32_t kO0SkewModeProbe = 2;
constexpr std::uint32_t kO0SkewModeSway = 3;
constexpr std::uint32_t kO0SkewModeSwayObj = 4;
constexpr std::uint32_t kO0SkewModeCount = 5;
std::atomic_uint32_t g_o0SkewProbeMode{kO0SkewModeSwayCam};
std::atomic_uint64_t g_o0SkewMeasured{};
std::atomic_uint64_t g_o0SkewApplied{};
std::atomic_uint64_t g_o0SkewSwayed{};
std::atomic_uint64_t g_o0SkewFiltered{};
std::atomic_uint64_t g_o0SkewInverseFailed{};
// SWAYOBJ: a sway run proved shadows on BOTH halves consume the
// 992 view-context payload while visible geometry on NEITHER half does, so
// the geometry view-projection is composed upstream (per-object constants).
// The main projection's depth constants q=-1.000017, qn=-0.200003 survive
// composition: any matrix A = M * P with affine M satisfies
// A[i][2] == -q*A[i][3] for rows 0..2 and A[3][2] == -q*A[3][3] + qn.
// SWAYOBJ scans every CPU-written buffer except the 992 payload for such
// blocks and sways them; whichever half rocks consumes CPU-composed
// geometry matrices, and the one-shot logs name the buffers.
std::atomic_uint64_t g_objSwayBlocks{};
std::atomic_uint64_t g_objSwayWrites{};
std::array<std::atomic_uintptr_t, 8> g_objSwayResources{};
// Left-eye injection at the proven geometry lane: the SWAYOBJ run
// moved terrain on both halves and named the 512-byte per-object rings at VS
// slot 0 (1-2 projection-composed blocks per write, equalO1AtO0 ~ 0, so the
// left/right phase tag is trustworthy for THESE buffers).  In PROBE mode,
// fingerprinted blocks written during the left phase get the opposite of the
// live right-eye skew: dP[2][0] = -refSkew, i.e. A[i][0] += refSkew*A[i][3].
std::atomic_uint64_t g_objSkewWrites{};
std::atomic_uint64_t g_objSkewBlocks{};
std::array<std::atomic_uintptr_t, 8> g_objSkewResources{};
// Per-phase observability: fingerprint writes seen with left/right TLS tags.
// The 992-byte view-context buffer's size is scene-dependent (some scenes
// have none), so the skew is derived directly from engine state
// (baseline/scale, exact), and these counters plus one-shot logs make a
// dead gate visible in any log.
std::atomic_uint64_t g_objSkewSeenLeft{};
std::atomic_uint64_t g_objSkewSeenRight{};
std::atomic_uint32_t g_objSkewPhaseLogged{};
// Patched blocks still match the fingerprint (patch touches column 0 only),
// so re-scanning bytes that survive between maps would accumulate skew.
// Only D3D11_MAP_WRITE_DISCARD maps are patched; anything else is counted
// and one-shot logged so a legitimate non-discard matrix lane shows up.
std::atomic_uint64_t g_objSkewSkippedNonDiscard{};
std::atomic_bool g_objSkewNonDiscardLogged{};
// Copy attribution: the 256-byte per-view camera CBs have writer=+0x0 (never
// CPU-mapped, never UpdateSubresource'd), so they are filled by GPU-side
// copies.  [CBCOPY] one-shots name the source buffer and engine caller so
// the main-path camera supply line is identified from any log.
std::atomic_uint32_t g_cbCopyLogged{};
std::array<std::atomic_uintptr_t, 6> g_o0SkewPatchedResources{};
struct MainCameraSkewRef
{
    SRWLOCK lock = SRWLOCK_INIT;
    float skew{};
    float position[3]{};
    std::uint64_t present{};
};
MainCameraSkewRef g_mainCamSkewRef{};

bool InvertMatrix4x4(const float* source, float* destination)
{
    double work[4][8]{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
            work[row][column] = source[row * 4 + column];
        work[row][4 + row] = 1.0;
    }
    for (int pivot = 0; pivot < 4; ++pivot)
    {
        int best = pivot;
        for (int row = pivot + 1; row < 4; ++row)
        {
            const double candidate = work[row][pivot] < 0.0
                ? -work[row][pivot] : work[row][pivot];
            const double current = work[best][pivot] < 0.0
                ? -work[best][pivot] : work[best][pivot];
            if (candidate > current)
                best = row;
        }
        const double pivotMagnitude = work[best][pivot] < 0.0
            ? -work[best][pivot] : work[best][pivot];
        if (pivotMagnitude < 1e-12)
            return false;
        if (best != pivot)
            for (int column = 0; column < 8; ++column)
            {
                const double swap = work[pivot][column];
                work[pivot][column] = work[best][column];
                work[best][column] = swap;
            }
        const double scale = 1.0 / work[pivot][pivot];
        for (int column = 0; column < 8; ++column)
            work[pivot][column] *= scale;
        for (int row = 0; row < 4; ++row)
        {
            if (row == pivot)
                continue;
            const double factor = work[row][pivot];
            for (int column = 0; column < 8; ++column)
                work[row][column] -= factor * work[pivot][column];
        }
    }
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            destination[row * 4 + column] =
                static_cast<float>(work[row][4 + column]);
    return true;
}

struct CopyWriterSample
{
    std::atomic_uintptr_t destination{};
    std::atomic_uintptr_t callerRva{};
    std::atomic_uint32_t size{};
};
constexpr std::size_t kCopyWriterSampleCount = 32768;
std::array<CopyWriterSample, kCopyWriterSampleCount> g_copyWriters{};

struct PoseRecorderCaller
{
    std::atomic_uintptr_t rva{};
    std::atomic_uint32_t total{};
    std::atomic_uint32_t bytes16{};
    std::atomic_uint32_t bytes32{};
    std::atomic_uint32_t bytes64{};
};
std::array<PoseRecorderCaller, 64> g_poseRecorderCallers{};
std::atomic_uint64_t g_poseRecorderHits{};

struct PassNameSample
{
    std::atomic_uint64_t key{};
    std::atomic_uint32_t ready{};
    char name[48]{};
    std::array<std::atomic_uint64_t, 3> calls{};
};
constexpr std::size_t kPassNameSampleCount = 512;
std::array<PassNameSample, kPassNameSampleCount> g_passNames{};
std::atomic_uint64_t g_passNamesDropped{};

struct OutputCallerSample
{
    std::atomic_uintptr_t rva{};
    std::array<std::atomic_uint64_t, 3> calls{};
};
constexpr std::size_t kOutputCallerSampleCount = 256;
std::array<OutputCallerSample, kOutputCallerSampleCount> g_outputCallers{};
std::atomic_uint64_t g_outputCallersDropped{};

struct DispatchCallerSample
{
    std::atomic_uint64_t key{};
    std::atomic_uint32_t ready{};
    std::uintptr_t rva{};
    std::uintptr_t shader{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
    std::array<std::atomic_uint64_t, 3> calls{};
};
constexpr std::size_t kDispatchCallerSampleCount = 512;
std::array<DispatchCallerSample, kDispatchCallerSampleCount> g_dispatchCallers{};
std::atomic_uint64_t g_dispatchCallersDropped{};
std::array<std::atomic_uint64_t, 3> g_probeResourceLastLogMs{};

constexpr std::size_t kProbeWeightResourceCount = 8;
std::array<std::atomic_uintptr_t, kProbeWeightResourceCount> g_probeWeightResources{};
std::array<std::atomic_uintptr_t, 3> g_lastProbeWeightResource{};

struct ProbeConsumerSample
{
    std::atomic_uint64_t key{};
    std::atomic_uint32_t ready{};
    std::uintptr_t shader{};
    std::uintptr_t resource{};
    std::uint32_t slot{};
    std::array<std::atomic_uint64_t, 3> calls{};
    std::array<std::atomic_uint64_t, 3> currentMatches{};
};
constexpr std::size_t kProbeConsumerSampleCount = 128;
std::array<ProbeConsumerSample, kProbeConsumerSampleCount> g_probeConsumers{};
std::atomic_uint64_t g_probeConsumersDropped{};
std::array<std::atomic_uint64_t, 3> g_draw4Calls{};
std::array<std::atomic_uint64_t, 3> g_draw4LastLogMs{};
std::array<std::atomic_uint64_t, 3> g_ambientSnapshotLastLogMs{};
SRWLOCK g_monoAmbientAoLock = SRWLOCK_INIT;
ID3D11ShaderResourceView* g_monoAmbientAoSrv{};
std::atomic_uint64_t g_monoAmbientAoLearned{};
std::array<std::atomic_uint64_t, 2> g_monoAmbientAoProofApplied{};
std::atomic_uint64_t g_monoAmbientAoProofMissed{};



CreateSwapChainFn g_realCreateSwapChain{};
CreateSwapChainForHwndFn g_realCreateSwapChainForHwnd{};
CreateSwapChainForCoreWindowFn g_realCreateSwapChainForCoreWindow{};
CreateSwapChainForCompositionFn g_realCreateSwapChainForComposition{};
CreateBufferFn g_realCreateBuffer{};
CreateTexture2DFn g_realCreateTexture2D{};
IDXGIAdapter3* g_leakAdapter{};

int ReadTrueOutput();
void LogPassNameTelemetry();
void LogOutputCallerTelemetry();
void LogDispatchCallerTelemetry();
void LogProbeConsumerTelemetry();
bool HookContextVtable(ID3D11DeviceContext* context);

using ActorLodBudgetFn = std::uint64_t(__fastcall*)(void*, std::uint8_t, char,
    std::uint8_t, char*, short, float);
ActorLodBudgetFn g_realActorLodBudget{};
using ActorLodManagerRunFn = void(__fastcall*)(void*);
ActorLodManagerRunFn g_realActorLodManagerRun{};
extern "C" void __fastcall RetailActorLodManagerRunPolicy(void* manager);
using ActorPointLodKernelFn = void(__fastcall*)(void*, void*, std::uint16_t);
ActorPointLodKernelFn g_realActorPointLodKernel{};
extern "C" void __fastcall RetailActorPointLodKernelPolicy(void* manager,
    void* entry, std::uint16_t index);
using ActorBoundedLodKernelFn = void(__fastcall*)(void*, void*, std::uint16_t);
ActorBoundedLodKernelFn g_realActorBoundedLodKernel{};
extern "C" void __fastcall RetailActorBoundedLodKernelPolicy(void* manager,
    void* entry, std::uint16_t index);

extern "C" std::uint64_t __fastcall RetailActorLodBudgetPolicy(void* table,
    std::uint8_t group, char currentLevel, std::uint8_t inputLevel,
    char* adjustment, short priority, float distance)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto callerRva = caller >= g_exeBase && caller < g_exeBase + kRetailImageSize
        ? caller - g_exeBase : 0;
    if (caller >= g_exeBase && caller < g_exeBase + kRetailImageSize)
    {
        for (auto& sample : g_actorLodCallers)
        {
            auto existing = sample.rva.load(std::memory_order_acquire);
            if (existing == 0 && !sample.rva.compare_exchange_strong(existing, callerRva,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            if (existing != 0 && existing != callerRva)
                continue;
            const auto mode = g_stereoEnabled.load(std::memory_order_relaxed) ? 1u : 0u;
            sample.levels[mode][inputLevel < 8 ? inputLevel : 7]
                .fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    auto effectiveInputLevel = inputLevel;
    if constexpr (kClampNearbyBoundedAnimationLod)
    {
        if (g_stereoEnabled.load(std::memory_order_relaxed) &&
            callerRva == kActorBoundedBudgetReturnRva && inputLevel >= 4 &&
            distance < kBoundedAnimationLodDistance)
        {
            effectiveInputLevel = 3;
            g_actorBoundedAnimationLodForced.fetch_add(1, std::memory_order_relaxed);
        }
    }
    auto result = g_realActorLodBudget
        ? g_realActorLodBudget(table, group, currentLevel, effectiveInputLevel,
            adjustment, priority, distance)
        : static_cast<std::uint64_t>(effectiveInputLevel);
    if (g_stereoEnabled.load(std::memory_order_relaxed) &&
        callerRva == kActorBoundedBudgetReturnRva &&
        distance < kNearLodRestoreDistance)
    {
        g_nearLodSeen.fetch_add(1, std::memory_order_relaxed);
        if (g_nearLodRestoreEnabled.load(std::memory_order_acquire) &&
            static_cast<std::uint8_t>(result) != 0)
        {
            result = 0;
            g_nearLodForced.fetch_add(1, std::memory_order_relaxed);
        }
    }
    (g_stereoEnabled.load(std::memory_order_acquire)
        ? g_actorLodBudgetStereoCalls : g_actorLodBudgetMonoCalls)
        .fetch_add(1, std::memory_order_relaxed);
    g_actorLodBudgetOutputCalls[ReadTrueOutput() & 1]
        .fetch_add(1, std::memory_order_relaxed);
    g_actorLodBudgetGroupCalls[group < 8 ? group : 7]
        .fetch_add(1, std::memory_order_relaxed);
    g_actorLodBudgetInputLevelCalls[inputLevel < 8 ? inputLevel : 7]
        .fetch_add(1, std::memory_order_relaxed);
    const auto resultLevel = static_cast<std::uint8_t>(result);
    g_actorLodBudgetResultLevelCalls[resultLevel < 8 ? resultLevel : 7]
        .fetch_add(1, std::memory_order_relaxed);
    std::size_t distanceBin{};
    if (distance >= 5.0f) ++distanceBin;
    if (distance >= 10.0f) ++distanceBin;
    if (distance >= 20.0f) ++distanceBin;
    if (distance >= 40.0f) ++distanceBin;
    if (distance >= 80.0f) ++distanceBin;
    if (distance >= 160.0f) ++distanceBin;
    if (distance >= 320.0f) ++distanceBin;
    g_actorLodDistanceBins[g_stereoEnabled.load(std::memory_order_relaxed) ? 1 : 0]
        [distanceBin].fetch_add(1, std::memory_order_relaxed);
    return result;
}



// Private-data sentinels are released by D3D when the owning resource is actually destroyed.
// This observes COM lifetime without touching the game's AddRef/Release calls.
constexpr GUID kLeakSentinelGuid =
    {0x48d8ef47, 0x49b4, 0x4ff6, {0x9d, 0xcf, 0xa7, 0x6e, 0x36, 0x0d, 0xa8, 0x51}};

void WriteLogLine(const char* message);

enum class LeakResourceKind : std::uint8_t
{
    Buffer,
    Texture,
};

class LeakResourceSentinel final : public IUnknown
{
public:
    LeakResourceSentinel(LeakResourceKind kind, std::uint64_t bytes, bool depth1024)
        : kind_(kind), bytes_(bytes), depth1024_(depth1024) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
    {
        if (!value)
            return E_POINTER;
        if (iid == __uuidof(IUnknown))
        {
            *value = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *value = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0)
        {
            if (kind_ == LeakResourceKind::Buffer)
            {
                g_buffersDestroyed.fetch_add(1, std::memory_order_relaxed);
                g_bufferBytesDestroyed.fetch_add(bytes_, std::memory_order_relaxed);
            }
            else
            {
                g_texturesDestroyed.fetch_add(1, std::memory_order_relaxed);
                g_textureBytesDestroyed.fetch_add(bytes_, std::memory_order_relaxed);
                if (depth1024_)
                {
                    const auto sample = g_depthDestroyStackSamples.fetch_add(1,
                        std::memory_order_relaxed) + 1;
                    if (sample <= 4)
                    {
                        void* frames[24]{};
                        const USHORT count = CaptureStackBackTrace(0, _countof(frames), frames, nullptr);
                        char stackLine[1792]{};
                        StringCchPrintfA(stackLine, _countof(stackLine),
                            "[TEXDESTROY] #%u frames=%u exe-rvas:", sample, count);
                        for (USHORT frame = 0; frame < count; ++frame)
                        {
                            const auto address = reinterpret_cast<std::uintptr_t>(frames[frame]);
                            if (address < g_exeBase || address >= g_exeBase + kRetailImageSize)
                                continue;
                            char suffix[48]{};
                            StringCchPrintfA(suffix, _countof(suffix), " 0x%llX",
                                static_cast<unsigned long long>(address - g_exeBase));
                            StringCchCatA(stackLine, _countof(stackLine), suffix);
                        }
                        WriteLogLine(stackLine);
                    }
                }
            }
            delete this;
        }
        return remaining;
    }

private:
    std::atomic_ulong refs_{1};
    LeakResourceKind kind_{};
    std::uint64_t bytes_{};
    bool depth1024_{};
};

void WriteLogLine(const char* message)
{
    AcquireSRWLockExclusive(&g_logLock);
    if (g_logHandle != INVALID_HANDLE_VALUE && message)
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char line[2048]{};
        const HRESULT lineResult = StringCchPrintfA(line, _countof(line),
            "%02u:%02u:%02u.%03u %s\r\n", now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, message);
        if (SUCCEEDED(lineResult))
        {
            DWORD written{};
            WriteFile(g_logHandle, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        }
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

void Log(const char* format, ...)
{
    if(QuietGameplayLog(format))return;
    char message[1792]{};
    va_list args;
    va_start(args, format);
    const HRESULT result = StringCchVPrintfA(message, _countof(message), format, args);
    va_end(args);
    if (SUCCEEDED(result))
        WriteLogLine(message);
}

void OpenLog()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;
    std::size_t slash = static_cast<std::size_t>(-1);
    for (std::size_t index = 0; index < length; ++index)
        if (path[index] == L'\\')
            slash = index;
    constexpr wchar_t name[] = L"ffxv-vr.log";
    if (slash == static_cast<std::size_t>(-1) || slash + _countof(name) >= _countof(path))
        return;
    std::memcpy(path + slash + 1, name, sizeof(name));
    // Keep the previous session: a hang followed by a relaunch used to
    // overwrite the only record of the hang.
    {
        constexpr wchar_t previous[] = L"ffxv-vr.prev.log";
        wchar_t previousPath[MAX_PATH]{};
        if (slash + _countof(previous) < _countof(previousPath))
        {
            std::memcpy(previousPath, path, (slash + 1) * sizeof(wchar_t));
            std::memcpy(previousPath + slash + 1, previous, sizeof(previous));
            MoveFileExW(path, previousPath, MOVEFILE_REPLACE_EXISTING);
        }
    }
    g_logHandle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Log("[DIAGNOSTICS] research=%s; camera recovery, pose association, stereo and HUD remain active",
        ResearchDiagnostics::Enabled ? "ON" : "OFF");
}

bool IsReadable(const void* address, std::size_t size)
{
    if (!address || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + size;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return end >= begin && end <= regionEnd;
}

bool IsWritable(const void* address, std::size_t size)
{
    if (!address || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;
    const DWORD access = info.Protect & 0xFF;
    if (access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
        access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
        return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + size;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return end >= begin && end <= regionEnd;
}

void RecordHotThread();

extern "C" void __fastcall RetailActorLodManagerRunPolicy(void* manager)
{
    RecordHotThread();
    if constexpr (!ResearchDiagnostics::Enabled)
    {
        if (g_realActorLodManagerRun) g_realActorLodManagerRun(manager);
        return;
    }
    const auto now = GetTickCount64();
    auto previous = g_actorLodViewLastLogMs.load(std::memory_order_acquire);
    if ((previous == 0 || now - previous >= 1000) &&
        g_actorLodViewLastLogMs.compare_exchange_strong(previous, now,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        std::uint32_t familyCounts[3]{};
        void* familyBases[3]{};
        std::uint64_t flagZero[3]{};
        std::uint64_t flagNonzero[3]{};
        std::uint32_t sampledFamily = 3;
        void* block{};
        void* cameraA{};
        void* cameraB{};
        float a0[4]{}, b0[4]{}, a1[4]{}, b1[4]{}, scales[4]{};
        __try
        {
            const auto* bytes = static_cast<const std::uint8_t*>(manager);
            for (std::uint32_t family = 0; family < 3; ++family)
            {
                const auto offset = static_cast<std::size_t>(family) * 0x10;
                familyCounts[family] = *reinterpret_cast<const std::uint32_t*>(bytes + offset + 8);
                const auto familyBase = *reinterpret_cast<std::uint8_t* const*>(bytes + offset);
                familyBases[family] = familyBase;
                if (sampledFamily == 3 && familyCounts[family] && familyBase &&
                    IsReadable(familyBase, 0x40))
                {
                    sampledFamily = family;
                    block = familyBase;
                }
            }
            constexpr std::size_t entryStrides[3] = {0x40, 0x30, 0x50};
            constexpr std::size_t flagOffsets[3] = {0x39, 0x29, 0x49};
            for (std::uint32_t family = 0; family < 3; ++family)
            {
                const auto blockCount = familyCounts[family] < 4096
                    ? familyCounts[family] : 4096;
                const auto* familyBase = static_cast<const std::uint8_t*>(familyBases[family]);
                for (std::uint32_t blockIndex = 0; familyBase && blockIndex < blockCount; ++blockIndex)
                {
                    const auto* workBlock = familyBase + static_cast<std::size_t>(blockIndex) * 0x240;
                    if (!IsReadable(workBlock + 0x228, 16))
                        continue;
                    const auto* entries = *reinterpret_cast<std::uint8_t* const*>(workBlock + 0x228);
                    const auto entryCount = *reinterpret_cast<const std::uint32_t*>(workBlock + 0x230);
                    const auto stride = entryStrides[family];
                    if (!entries || entryCount > 100000 ||
                        !IsReadable(entries, static_cast<std::size_t>(entryCount) * stride))
                        continue;
                    for (std::uint32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex)
                    {
                        const auto value = entries[static_cast<std::size_t>(entryIndex) * stride +
                            flagOffsets[family]];
                        (value ? flagNonzero[family] : flagZero[family])++;
                    }
                }
            }
            if (block)
            {
                const auto* blockBytes = static_cast<const std::uint8_t*>(block);
                cameraA = *reinterpret_cast<void* const*>(blockBytes + 0x00);
                cameraB = *reinterpret_cast<void* const*>(blockBytes + 0x08);
                std::memcpy(scales, blockBytes + 0x18, sizeof(scales));
                if (IsReadable(cameraA, sizeof(a0) + sizeof(a1)))
                {
                    std::memcpy(a0, cameraA, sizeof(a0));
                    std::memcpy(a1, static_cast<const std::uint8_t*>(cameraA) + 0x10, sizeof(a1));
                }
                if (IsReadable(cameraB, sizeof(b0) + sizeof(b1)))
                {
                    std::memcpy(b0, cameraB, sizeof(b0));
                    std::memcpy(b1, static_cast<const std::uint8_t*>(cameraB) + 0x10, sizeof(b1));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            sampledFamily = 3;
            block = nullptr;
            cameraA = nullptr;
            cameraB = nullptr;
        }
        Log("[LODVIEW] stereo=%d manager=%p families=%u/%u/%u sample=%u block=%p cams=%p/%p "
            "a0=%.3f,%.3f,%.3f a1=%.3f,%.3f,%.3f b0=%.3f,%.3f,%.3f b1=%.3f,%.3f,%.3f "
            "scale=%.5f/%.5f/%.5f/%.5f flagsZero=%llu/%llu/%llu flagsSet=%llu/%llu/%llu",
            g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0, manager,
            familyCounts[0], familyCounts[1], familyCounts[2], sampledFamily,
            block, cameraA, cameraB,
            a0[0], a0[1], a0[2], a1[0], a1[1], a1[2],
            b0[0], b0[1], b0[2], b1[0], b1[1], b1[2],
            scales[0], scales[1], scales[2], scales[3],
            static_cast<unsigned long long>(flagZero[0]),
            static_cast<unsigned long long>(flagZero[1]),
            static_cast<unsigned long long>(flagZero[2]),
            static_cast<unsigned long long>(flagNonzero[0]),
            static_cast<unsigned long long>(flagNonzero[1]),
            static_cast<unsigned long long>(flagNonzero[2]));
    }
    if (g_realActorLodManagerRun)
        g_realActorLodManagerRun(manager);
}

extern "C" void __fastcall RetailActorPointLodKernelPolicy(void* manager,
    void* entry, std::uint16_t index)
{
    if constexpr (!ResearchDiagnostics::Enabled)
    {
        if (g_realActorPointLodKernel) g_realActorPointLodKernel(manager, entry, index);
        return;
    }
    const auto mode = g_stereoEnabled.load(std::memory_order_acquire) ? 1u : 0u;
    __try
    {
        const auto* bytes = static_cast<const std::uint8_t*>(entry);
        if (bytes && IsReadable(bytes + 0x43, 7))
        {
            (bytes[0x49] ? g_actorPointFallbackSet[mode] : g_actorPointFallbackZero[mode])
                .fetch_add(1, std::memory_order_relaxed);
            (bytes[0x44] ? g_actorPointStateSet[mode] : g_actorPointStateZero[mode])
                .fetch_add(1, std::memory_order_relaxed);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (g_realActorPointLodKernel)
        g_realActorPointLodKernel(manager, entry, index);
}

extern "C" void __fastcall RetailActorBoundedLodKernelPolicy(void* manager,
    void* entry, std::uint16_t index)
{
    const auto mode = g_stereoEnabled.load(std::memory_order_acquire) ? 1u : 0u;
    std::uint8_t* fallbackFlag{};
    std::uint8_t savedFallback{};
    std::uint8_t savedAck{};
    bool fallbackOverridden{};
    bool ackOverridden{};
    __try
    {
        auto* bytes = static_cast<std::uint8_t*>(entry);
        if (ResearchDiagnostics::Enabled && bytes && IsReadable(bytes, 0x40) &&
            (g_actorBoundedByteSampleSequence[mode].fetch_add(
                1, std::memory_order_relaxed) & 31) == 0)
        {
            for (std::size_t offset = 0; offset < 0x40; ++offset)
            {
                const auto value = bytes[offset];
                auto& counter = value == 0 ? g_actorBoundedByteZero[mode][offset]
                    : value == 1 ? g_actorBoundedByteOne[mode][offset]
                    : g_actorBoundedByteOther[mode][offset];
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (bytes && IsReadable(bytes + 0x34, 6))
        {
            (bytes[0x39] ? g_actorBoundedFallbackSet[mode] : g_actorBoundedFallbackZero[mode])
                .fetch_add(1, std::memory_order_relaxed);
            (bytes[0x34] ? g_actorBoundedState34Set[mode] : g_actorBoundedState34Zero[mode])
                .fetch_add(1, std::memory_order_relaxed);
            (bytes[0x36] ? g_actorBoundedState36Set[mode] : g_actorBoundedState36Zero[mode])
                .fetch_add(1, std::memory_order_relaxed);
            if constexpr (kForceBoundedFallbackInStereo)
            {
                // The kernel enqueues an actor for downstream animation work
                // only when the requested/acknowledged pair DISAGREES:
                //     ... || entry[0x38] != entry[0x39]
                // The invariant forces +0x39 = 1 only when it is already 0,
                // so an actor sitting at 38==39==1 is not queued.  A frozen
                // conversation NPC sits there (req39=1 ack38=1, queued=0);
                // forcing the mismatch does queue it (ackForced ~50k/s) but it
                // still does not animate, so the work queue is not the
                // mechanism and the invariant is kept as is.
                if (mode == 1 && bytes[0x39] == 0 && IsWritable(bytes + 0x38, 2) &&
                    g_boundedFallbackRuntimeEnabled.load(std::memory_order_acquire))
                {
                    fallbackFlag = bytes + 0x39;
                    savedFallback = *fallbackFlag;
                    savedAck = bytes[0x38];
                    // Keep the proven semantic (+0x39 = 1 = bounded path) and
                    // additionally guarantee 0x38 != 0x39 so the kernel queues
                    // this actor for animation work this frame.
                    *fallbackFlag = 1;
                    fallbackOverridden = true;
                    g_actorBoundedFallbackForced.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (g_realActorBoundedLodKernel)
        g_realActorBoundedLodKernel(manager, entry, index);

    if constexpr (ResearchDiagnostics::Enabled)
    {
        // [ACTOBJ] Change-census on the nearest actor's own object.
        __try
        {
            auto* entryBytes = static_cast<std::uint8_t*>(entry);
            if (entryBytes && IsReadable(entryBytes + 0x28, sizeof(float)) &&
                IsReadable(entryBytes + 0x20, sizeof(void*)))
            {
                const auto distance = *reinterpret_cast<const float*>(entryBytes + 0x28);
                auto* obj = *reinterpret_cast<std::uint8_t* const*>(entryBytes + 0x20);
                const auto now = GetTickCount();
                auto last = g_actObjLastLogMs.load(std::memory_order_acquire);
                if (obj && distance > 0.0f && distance < 6.0f &&
                    IsReadable(obj, kActObjWindow) && now - last >= 1000 &&
                    g_actObjLastLogMs.compare_exchange_strong(last, now,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    const auto objAddr = reinterpret_cast<std::uintptr_t>(obj);
                    const auto tracked = g_actObjTracked.exchange(objAddr,
                        std::memory_order_acq_rel);
                    if (tracked == objAddr && g_actObjHavePrev.load(std::memory_order_acquire))
                    {
                        char changed[320]{};
                        std::size_t used = 0, count = 0;
                        for (std::size_t offset = 0; offset + 4 <= kActObjWindow; offset += 4)
                        {
                            if (std::memcmp(g_actObjPrev + offset, obj + offset, 4) == 0)
                                continue;
                            ++count;
                            if (used + 8 < sizeof(changed))
                            {
                                StringCchPrintfA(changed + used, 8, "%03zX ", offset);
                                used = std::strlen(changed);
                            }
                        }
                        Log("[ACTOBJ] stereo=%d obj=%p dist=%.2f changedDwords=%zu [%s]",
                            g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0,
                            obj, distance, count, changed);
                    }
                    std::memcpy(g_actObjPrev, obj, kActObjWindow);
                    g_actObjHavePrev.store(true, std::memory_order_release);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        // [NEARACT] After the kernel has run, the entry holds this frame's distance
        // and freshly decided state.  Track the closest actor and report it once a
        // second: level (+0x33), flag (+0x35), mode (+0x36), and the queue
        // handshake pair (+0x38 requested / +0x39 acknowledged).  A conversation
        // partner is the nearest actor, so this is its row.
        __try
        {
            auto* bytes = static_cast<std::uint8_t*>(entry);
            if (bytes && IsReadable(bytes + 0x28, sizeof(float)) && IsReadable(bytes + 0x33, 7))
            {
                const auto distance = *reinterpret_cast<const float*>(bytes + 0x28);
                if (distance > 0.0f && distance < 4000.0f)
                {
                    const std::uint64_t state =
                        (static_cast<std::uint64_t>(bytes[0x33]) << 32) |
                        (static_cast<std::uint64_t>(bytes[0x35]) << 24) |
                        (static_cast<std::uint64_t>(bytes[0x36]) << 16) |
                        (static_cast<std::uint64_t>(bytes[0x38]) << 8) |
                        static_cast<std::uint64_t>(bytes[0x39]);
                    std::uint32_t distanceBits{};
                    std::memcpy(&distanceBits, &distance, sizeof(distanceBits));
                    auto best = g_nearActBestDistanceBits.load(std::memory_order_acquire);
                    float bestDistance{};
                    std::memcpy(&bestDistance, &best, sizeof(bestDistance));
                    if (best == 0 || distance < bestDistance)
                    {
                        g_nearActBestDistanceBits.store(distanceBits, std::memory_order_release);
                        g_nearActBestState.store(state, std::memory_order_release);
                    }
                    const auto now = GetTickCount();
                    auto last = g_nearActLastLogMs.load(std::memory_order_acquire);
                    if (now - last >= 1000 &&
                        g_nearActLastLogMs.compare_exchange_strong(last, now,
                            std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        const auto reportBits =
                            g_nearActBestDistanceBits.exchange(0, std::memory_order_acq_rel);
                        const auto reportState =
                            g_nearActBestState.load(std::memory_order_acquire);
                        float reportDistance{};
                        std::memcpy(&reportDistance, &reportBits, sizeof(reportDistance));
                        Log("[NEARACT] stereo=%d nearest=%.2f level=%u flag=%u mode=%u "
                            "req39=%u ack38=%u queued=%d",
                            g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0,
                            reportDistance,
                            static_cast<unsigned>((reportState >> 32) & 0xFF),
                            static_cast<unsigned>((reportState >> 24) & 0xFF),
                            static_cast<unsigned>((reportState >> 16) & 0xFF),
                            static_cast<unsigned>(reportState & 0xFF),
                            static_cast<unsigned>((reportState >> 8) & 0xFF),
                            ((reportState >> 8) & 0xFF) != (reportState & 0xFF) ? 1 : 0);
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    if (fallbackOverridden && fallbackFlag && !kHoldBoundedFallbackThroughDownstream)
    {
        __try
        {
            if (ackOverridden)
                fallbackFlag[-1] = savedAck;
            *fallbackFlag = savedFallback;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

// Post-effect chain eye-1 duplicate skip.  In native stereo the output-0
// compatibility alias makes these once-per-frame machines run once per OUTPUT;
// output 1's pass is a duplicate over output-0 state (its own bank is null by
// retail design, see FUN_14ead4120).  Skip only the eye-1 call, only while
// stereo and the toggle are on; mono behavior is bit-identical.
bool PostChainShouldSkip(std::size_t index)
{
    if (!kPostChainSkippable[index] ||
        !g_stereoEnabled.load(std::memory_order_acquire) ||
        !g_postChainSkipEnabled.load(std::memory_order_acquire))
    {
        g_postChainExecuted[index].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (ReadTrueOutput() == 1)
    {
        g_postChainSkipped[index].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_postChainExecuted[index].fetch_add(1, std::memory_order_relaxed);
    return false;
}

using PostChainOneArgFn = void(__fastcall*)(void*);
using PostChainThreeArgFn = void(__fastcall*)(void*, void*, void*);
PostChainOneArgFn g_realPostChain0{};
PostChainThreeArgFn g_realPostChain1{};
PostChainOneArgFn g_realPostChain2{};
PostChainOneArgFn g_realPostChain3{};
PostChainOneArgFn g_realPostChain4{};
PostChainOneArgFn g_realPostChain5{};

extern "C" void __fastcall RetailPostChain0Policy(void* a)
{
    if (PostChainShouldSkip(0))
        return;
    if (g_realPostChain0)
        g_realPostChain0(a);
}

extern "C" void __fastcall RetailPostChain1Policy(void* a, void* b, void* c)
{
    if (PostChainShouldSkip(1))
        return;
    if (g_realPostChain1)
        g_realPostChain1(a, b, c);
}

extern "C" void __fastcall RetailPostChain2Policy(void* a)
{
    if (PostChainShouldSkip(2))
        return;
    if (g_realPostChain2)
        g_realPostChain2(a);
}

extern "C" void __fastcall RetailPostChain3Policy(void* a)
{
    if (PostChainShouldSkip(3))
        return;
    if (g_realPostChain3)
        g_realPostChain3(a);
}

extern "C" void __fastcall RetailPostChain4Policy(void* a)
{
    if (PostChainShouldSkip(4))
        return;
    if (g_realPostChain4)
        g_realPostChain4(a);
}

extern "C" void __fastcall RetailPostChain5Policy(void* a)
{
    if (PostChainShouldSkip(5))
        return;
    if (g_realPostChain5)
        g_realPostChain5(a);
}

using BankRotatorFn = void(__fastcall*)(void*);
BankRotatorFn g_realBankRotator{};

using PassRegistrarFn = void*(__fastcall*)(void*, void*, const char*, void*, void*);
PassRegistrarFn g_realPassRegistrar{};

void PassCensusRecord(const char* name)
{
    char local[48]{};
    bool ok = false;
    __try
    {
        if (name && IsReadable(name, 8))
        {
            for (std::size_t i = 0; i + 1 < sizeof(local); ++i)
            {
                const char c = name[i];
                if (c == '\0')
                    break;
                if (c < 0x20 || c > 0x7E)
                    return;
                local[i] = c;
            }
            ok = local[0] != '\0';
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    if (!ok)
        return;
    // [GBUF] The depth/velocity census attributes every target's draws to the
    // pass running right now; it runs in the play build, unlike the rest.
    GBufferCensus::OnPass(local);
    DlssProbe::OnPass(local);
    if constexpr (!ResearchDiagnostics::Enabled) return;
    // [PASSDRAW] Every announcement, so draws can be attributed to the pass
    // running right now.  The census below only logs a name the first time.
    SceneCensus::SetCurrentPass(local);
    const bool stereo = g_stereoEnabled.load(std::memory_order_acquire);
    const bool ever = g_stereoEverEnabled.load(std::memory_order_acquire);
    // modes: 0 cleanmono, 1 stereo-o0, 2 stereo-o1, 3 postmono
    const std::uint32_t mode = stereo ? (ReadTrueOutput() == 1 ? 2u : 1u)
        : (ever ? 3u : 0u);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char* p = local; *p; ++p)
        hash = (hash ^ static_cast<std::uint8_t>(*p)) * 1099511628211ULL;
    const std::uint64_t key = (hash & ~0x3ULL) | mode;
    auto slot = static_cast<std::size_t>((key >> 2) & (kPassCensusSlots - 1));
    for (std::size_t probe = 0; probe < kPassCensusSlots; ++probe)
    {
        auto& cell = g_passCensusSeen[(slot + probe) & (kPassCensusSlots - 1)];
        std::uint64_t expected = 0;
        const auto current = cell.load(std::memory_order_acquire);
        if (current == key)
            return;
        if (current == 0 && cell.compare_exchange_strong(expected, key,
                std::memory_order_acq_rel))
        {
            static const char* const kModeNames[4] = {
                "cleanmono", "o0", "o1", "postmono"};
            Log("[PASSCEN] mode=%s pass=%s", kModeNames[mode], local);
            return;
        }
    }
}

extern "C" void* __fastcall RetailPassRegistrarPolicy(void* a, void* b,
    const char* name, void* d, void* e)
{
    PassCensusRecord(name);
    return g_realPassRegistrar ? g_realPassRegistrar(a, b, name, d, e) : nullptr;
}

using SetCurrentOutputFn = void(__fastcall*)(void*, std::uint32_t, std::uint8_t);
SetCurrentOutputFn g_realSetCurrentOutput{};
void* AllocateRelayNear(std::uintptr_t site, std::uintptr_t target);

extern "C" void __fastcall RetailFiberOutputRestorePolicy(void* mgr,
    std::uint32_t value, std::uint8_t flag)
{
    (value == 0 ? g_outsetValue0 : value == 1 ? g_outsetValue1 : g_outsetOther)
        .fetch_add(1, std::memory_order_relaxed);
    const bool stereo = g_stereoEnabled.load(std::memory_order_acquire);
    if (!stereo && value != 0)
    {
        // Post-stereo heal: outside stereo no fiber may restore a nonzero
        // output.  In a never-stereo session this never fires.
        value = 0;
        const auto healed = g_outsetHealed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (healed <= 4)
            Log("[OUTSET] healed poisoned fiber output (mono) #%llu",
                static_cast<unsigned long long>(healed));
    }
    else if (stereo && value == 1 &&
        g_stereoFiberClampEnabled.load(std::memory_order_acquire) &&
        GetCurrentThreadId() != g_phaseThreadId.load(std::memory_order_acquire))
    {
        // In-stereo clamp: only the per-output phase thread may run branded as
        // output 1; builder fibers on worker threads stay output 0 so their
        // per-output lookups hit constructed (output-0) data.
        value = 0;
        const auto clamped = g_outsetClamped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (clamped <= 4)
            Log("[OUTSET] clamped worker fiber output=1 -> 0 (stereo) #%llu",
                static_cast<unsigned long long>(clamped));
    }
    if (g_realSetCurrentOutput)
        g_realSetCurrentOutput(mgr, value, flag);
}

bool WriteRelativeJumpViaRelay(CodePatch& patch, std::uintptr_t site,
    std::uintptr_t thunk, void** relayStorage);

extern "C" void __fastcall RetailQualityMaskApplyPolicy(const std::uint8_t* blob,
    void* ctx)
{
    RecordHotThread();
    // The applier has TWO reduction channels:
    //   (a) blob[1] != 0  -> the big preset (probe relight off, viewFlags
    //       bits 0+2 cleared, feature-mask disables).  Never fires in retail.
    //   (b) UNCONDITIONAL, every frame in multi-output: four feature flags are
    //       rewritten as `saved & gate`, gate = blob[3] (three of them) and
    //       blob[4] (the fourth):
    //         +0xE9616E0 -> obj(+0x58)+0x1E1
    //         +0xE960F00 -> obj(+0x58)+0x1E0
    //         +0xE962A30 -> subsystem+0xB9
    //         +0xE9622A0 -> subsystem+0xB8
    //       A zero gate force-disables all four every frame; mono never runs
    //       this path, which hides world-space markers/interact icons in
    //       stereo.  Fix = make the gates 0xFF so the AND is a
    //       no-op and each feature keeps its own saved value.
    std::uint8_t local[kQualityBlobSize];
    bool substituted = false;
    if (g_qualityKeepEnabled.load(std::memory_order_acquire) && blob)
    {
        __try
        {
            if (IsReadable(blob, kQualityBlobSize))
            {
                static std::atomic<std::uint64_t> lastSignature{~0ull};
                std::uint64_t signature = 0;
                for (std::size_t i = 0; i < 8; ++i)
                    signature = (signature << 8) | blob[i];
                if (lastSignature.exchange(signature, std::memory_order_acq_rel)
                        != signature)
                {
                    char hex[kQualityBlobSize * 3 + 1]{};
                    for (std::size_t i = 0; i < kQualityBlobSize; ++i)
                        StringCchPrintfA(hex + i * 3, 4, "%02X ", blob[i]);
                    Log("[QMASK] blob stereo=%d bytes=%s (gates: [3]=%02X [4]=%02X)",
                        g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0,
                        hex, blob[3], blob[4]);
                }
                if (blob[1] != 0 || blob[3] != 0xFF || blob[4] != 0xFF)
                {
                    std::memcpy(local, blob, kQualityBlobSize);
                    local[1] = 0;
                    local[3] = 0xFF;
                    local[4] = 0xFF;
                    substituted = true;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            substituted = false;
        }
    }
    (substituted ? g_qualityKeepHits : g_qualityPassthrough)
        .fetch_add(1, std::memory_order_relaxed);
    if (substituted && g_qualityKeepHits.load(std::memory_order_relaxed) <= 4)
        Log("[QMASK] substituted apply #%llu (blob[1]->0, gates [3]/[4]->FF)",
            static_cast<unsigned long long>(
                g_qualityKeepHits.load(std::memory_order_relaxed)));
    if (g_realQualityMask)
        g_realQualityMask(substituted ? local : blob, ctx);
}

// [VRESTORE] Intro "lights/shadows blinking" (captured frames: one
// eye at a time flips for 1-2 frames to a darker lighting state - ground
// cover and white buildings lose their ambient fill, geometry and shadows
// unchanged = probe/ambient lighting off).  The engine does save -> reduce ->
// restore of the view quality state per output; FUN_14679e7c0 (+0x679E7C0,
// reached via vtable data +0x542A614 and thunk +0x47F9B0) RESTORES it from a
// saved 0x13-byte blob: relight master = blob[6], ctx+0x1DF8 = blob[5],
// viewFlags bit2 = blob[8], bit0 = blob[9], six mask6e0 bits = blob[7,10..14],
// then four setters on FUN_14e8e2ef0(deferredBase) with blob[15..18].  A blob
// saved while reduced restores "probes off".  Correcting the flags later
// inside the probe producer only works when it runs after this restore for
// that eye, which cutscenes reorder.  So the restore is reimplemented
// exactly, then (stereo or after stereo, fix enabled) bits 0+2 set,
// relight 1, mask6e0 bit31 clear - the measured mono baseline - inside the
// restore itself.
constexpr std::uintptr_t kViewRestoreRva = 0x0679E7C0;
constexpr std::array<std::uint8_t, 16> kViewRestorePrefix{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB, 0x48, 0x8B, 0x0D};
CodePatch g_viewRestorePatch{};
std::atomic<std::uint64_t> g_viewRestoreCalls{}, g_viewRestoreEnforced{};
void __fastcall ViewRestoreReplacement(const std::uint8_t* incoming, std::uint8_t* ctx)
{
    g_viewRestoreCalls.fetch_add(1, std::memory_order_relaxed);
    // [BLOBCHECK] A real saved state holds only small values (0..2 in every
    // byte; clean log: 02 00 01 01 01 02 01 00 01 01 00 00 00 00 00 01 00 00 00).  In stereo the engine also
    // calls this restore with a pointer into unrelated memory (logged:
    // "63 75 64 61 5F 00 ... 01 ..." = the text "cuda_"), whose zero bytes are
    // what switched probe lighting off in one eye - the missing reducer.
    // Such a restore is skipped: the state stays as the
    // last real restore left it.
    bool valid = true;
    for (int i = 0; i < 0x13 && valid; ++i)
        valid = incoming[i] <= 2;
    if (!valid)
    {
        static std::atomic<std::uint64_t> s_skipped{};
        const auto n = s_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 6 || (n % 3600) == 0)
        {
            char a[0x13 * 3 + 1]{};
            for (int i = 0; i < 0x13; ++i)
                StringCchPrintfA(a + i * 3, 4, "%02X ", incoming[i]);
            Log("[BLOBCHECK] skipped a restore from non-settings memory (#%llu): %s",
                static_cast<unsigned long long>(n), a);
        }
        return;
    }
    const std::uint8_t* blob = incoming;
    const auto deferredBase = *reinterpret_cast<std::uint8_t* const*>(g_exeBase + kDeferredCtxGlobalRva);
    if (deferredBase)
        deferredBase[kProbeRelightMasterOffset] = blob[6];
    ctx[0x1DF8] = blob[5];
    auto& mask = *reinterpret_cast<std::uint64_t*>(ctx + kFeatureMask64Offset);
    auto& flags = *reinterpret_cast<std::uint32_t*>(ctx + kProbeViewFlagsOffset);
    auto bit64 = [&](std::uint8_t on, std::uint64_t bit) { if (on) mask |= bit; else mask &= ~bit; };
    bit64(blob[7], 0x400000ull);
    if (blob[8]) flags |= 4u; else flags &= ~4u;
    if (blob[9]) flags |= 1u; else flags &= ~1u;
    bit64(blob[10], 0x400000000ull);
    bit64(blob[11], 0x8000000000ull);
    bit64(blob[12], 0x10000000000ull);
    bit64(blob[13], 0x80000000ull);
    bit64(blob[14], 0x200000000ull);
    using GetFn = void*(__fastcall*)(void*);
    using SetFn = void(__fastcall*)(void*, std::uint8_t);
    void* target = reinterpret_cast<GetFn>(g_exeBase + 0x0E8E2EF0)(deferredBase);
    if (target)
    {
        reinterpret_cast<SetFn>(g_exeBase + 0x0E9616E0)(target, blob[15]);
        reinterpret_cast<SetFn>(g_exeBase + 0x0E960F00)(target, blob[16]);
        reinterpret_cast<SetFn>(g_exeBase + 0x0E962A30)(target, blob[17]);
        reinterpret_cast<SetFn>(g_exeBase + 0x0E9622A0)(target, blob[18]);
    }
    if (g_viewFlagsFixEnabled.load(std::memory_order_acquire) &&
        (g_stereoEnabled.load(std::memory_order_acquire) || g_everStereo.load(std::memory_order_acquire)))
    {
        const bool reduced = (flags & 5u) != 5u || (mask & 0x80000000ull) != 0 ||
            (deferredBase && deferredBase[kProbeRelightMasterOffset] == 0);
        flags |= 5u;
        mask &= ~0x80000000ull;
        if (deferredBase)
            deferredBase[kProbeRelightMasterOffset] = 1;
        if (reduced)
        {
            const auto n = g_viewRestoreEnforced.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 6 || (n % 3600) == 0)
                Log("[VRESTORE] engine restored a REDUCED lighting state (blob relight=%u bit2=%u bit0=%u bit31=%u); full lighting enforced (#%llu of %llu restores)",
                    blob[6], blob[8], blob[9], blob[13], static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(g_viewRestoreCalls.load(std::memory_order_relaxed)));
        }
    }
}

extern "C" void __fastcall RetailProbeWeightsPolicy(void* obj, void* p2,
    void* p3, std::uint8_t p4)
{
    RecordHotThread();
    const bool stereo = g_stereoEnabled.load(std::memory_order_acquire);
    if (stereo)
        g_everStereo.store(true, std::memory_order_release);
    const std::uint32_t modeIdx = stereo ? 1u
        : g_everStereo.load(std::memory_order_acquire) ? 2u : 0u;
    if constexpr (ResearchDiagnostics::Enabled)
    {
        g_probewCalls[modeIdx].fetch_add(1, std::memory_order_relaxed);
        g_probewCtx.store(reinterpret_cast<std::uintptr_t>(p3), std::memory_order_release);
    }
    if (g_viewFlagsFixEnabled.load(std::memory_order_acquire) &&
        (stereo || g_everStereo.load(std::memory_order_acquire)))
    {
        __try
        {
            const auto ctxAddr = reinterpret_cast<std::uintptr_t>(p3);
            if (ctxAddr)
            {
                auto* flags = reinterpret_cast<std::uint32_t*>(
                    ctxAddr + kProbeViewFlagsOffset);
                if (IsReadable(flags, 4) && (*flags & 5u) != 5u)
                {
                    *flags |= 5u;
                    g_midFlagsCorrections.fetch_add(1, std::memory_order_relaxed);
                }
                auto* mask = reinterpret_cast<std::uint64_t*>(
                    ctxAddr + kFeatureMask64Offset);
                if (IsReadable(mask, 8) && (*mask & 0x80000000ull) != 0)
                {
                    *mask &= ~0x80000000ull;
                    g_maskBitCorrections.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const auto globalAddr = g_exeBase + kDeferredCtxGlobalRva;
            std::uintptr_t deferredBase = 0;
            if (IsReadable(reinterpret_cast<void*>(globalAddr), sizeof(std::uintptr_t)))
                deferredBase = *reinterpret_cast<std::uintptr_t*>(globalAddr);
            if (deferredBase)
            {
                auto* relight = reinterpret_cast<std::uint8_t*>(
                    deferredBase + kProbeRelightMasterOffset);
                if (IsReadable(relight, 1) && *relight == 0)
                {
                    *relight = 1;
                    g_relightCorrections.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    if constexpr (ResearchDiagnostics::Enabled)
    {
    const auto objAddr = reinterpret_cast<std::uintptr_t>(obj);
    for (auto& slot : g_probewObjs)
    {
        std::uintptr_t expected = 0;
        const auto current = slot.load(std::memory_order_acquire);
        if (current == objAddr)
            break;
        if (current == 0 && slot.compare_exchange_strong(expected, objAddr,
                std::memory_order_acq_rel))
            break;
    }

    std::uint32_t count = 0xFFFFFFFF;
    std::uint32_t valid = 0xFF;
    __try
    {
        const auto* bytes = static_cast<const std::uint8_t*>(obj);
        if (bytes && IsReadable(bytes + 0x60, 8))
        {
            count = *reinterpret_cast<const std::uint32_t*>(bytes + 0x60);
            valid = bytes[0x64];
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    const auto now = GetTickCount();
    auto lastMs = g_probewLastLogMs.load(std::memory_order_acquire);
    if (now - lastMs >= 1000 && g_probewLastLogMs.compare_exchange_strong(lastMs, now,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        static const char* const kModes[3] = {"cleanmono", "stereo", "postmono"};
        Log("[PROBEW] mode=%s obj=%p count=%u valid=%u recompute=%d",
            kModes[modeIdx], obj, count, valid,
            (count != 0 || valid == 0) ? 1 : 0);
    }
    }
    if (g_realProbeWeights)
        g_realProbeWeights(obj, p2, p3, p4);
}

bool BankRotatorSnapshot(void* ctx, std::uintptr_t* bindingsOut,
    std::array<std::uintptr_t, kBankRotatorGuardSlots.size()>& values)
{
    __try
    {
        const auto ctxAddr = reinterpret_cast<std::uintptr_t>(ctx);
        if (!IsReadable(reinterpret_cast<void*>(ctxAddr + kBindingsPointerCtxOffset),
                sizeof(std::uintptr_t)))
            return false;
        const auto holder = *reinterpret_cast<std::uintptr_t*>(
            ctxAddr + kBindingsPointerCtxOffset);
        if (!holder || !IsReadable(reinterpret_cast<void*>(holder + 8), sizeof(std::uintptr_t)))
            return false;
        const auto bindings = *reinterpret_cast<std::uintptr_t*>(holder + 8);
        if (!bindings ||
            !IsReadable(reinterpret_cast<void*>(bindings + kBankRotatorGuardSlots.front()),
                kBankRotatorGuardSlots.back() - kBankRotatorGuardSlots.front() +
                    sizeof(std::uintptr_t)))
            return false;
        for (std::size_t i = 0; i < kBankRotatorGuardSlots.size(); ++i)
            values[i] = *reinterpret_cast<std::uintptr_t*>(
                bindings + kBankRotatorGuardSlots[i]);
        *bindingsOut = bindings;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void BankRotatorRestore(std::uintptr_t bindings,
    const std::array<std::uintptr_t, kBankRotatorGuardSlots.size()>& values)
{
    __try
    {
        if (!IsWritable(reinterpret_cast<void*>(bindings + kBankRotatorGuardSlots.front()),
                kBankRotatorGuardSlots.back() - kBankRotatorGuardSlots.front() +
                    sizeof(std::uintptr_t)))
            return;
        for (std::size_t i = 0; i < kBankRotatorGuardSlots.size(); ++i)
            *reinterpret_cast<std::uintptr_t*>(bindings + kBankRotatorGuardSlots[i]) =
                values[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

extern "C" void __fastcall RetailBankRotatorPolicy(void* ctx)
{
    const bool guardWanted = g_stereoEnabled.load(std::memory_order_acquire) &&
        g_bankRotGuardEnabled.load(std::memory_order_acquire) &&
        ReadTrueOutput() == 1;
    if (!guardWanted)
    {
        g_bankRotEye0.fetch_add(1, std::memory_order_relaxed);
        if (g_realBankRotator)
            g_realBankRotator(ctx);
        return;
    }
    std::uintptr_t bindings{};
    std::array<std::uintptr_t, kBankRotatorGuardSlots.size()> saved{};
    const bool snapshotOk = BankRotatorSnapshot(ctx, &bindings, saved);
    if (!snapshotOk)
        g_bankRotGuardFailed.fetch_add(1, std::memory_order_relaxed);
    if (g_realBankRotator)
        g_realBankRotator(ctx);
    if (snapshotOk)
    {
        BankRotatorRestore(bindings, saved);
        const auto guarded = g_bankRotGuarded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (guarded <= 4)
            Log("[BANKROT] guarded eye-1 rotation #%llu bindings=%p",
                static_cast<unsigned long long>(guarded),
                reinterpret_cast<void*>(bindings));
    }
}

template <std::size_t N>
bool Matches(std::uintptr_t address, const std::array<std::uint8_t, N>& expected)
{
    return IsReadable(reinterpret_cast<const void*>(address), N) &&
        std::memcmp(reinterpret_cast<const void*>(address), expected.data(), N) == 0;
}

// [HISTFIX] Stereo->AER crashed at ffxv_s.exe+0xEBC2514
// (ScreenSpaceReflectionManager "reprojectedPreviousFrame" reading a null
// temporalAALightingTexture, shared targets +0x620).  In multi-output mode the
// engine keeps each eye's TAA history in that eye's view state: output END
// saves it (+0x4816E0 -> FUN_14e8e30c0, which zeroes the shared slot) and
// output BEGIN restores it (this function -> FUN_14e8e3580, which also
// allocates a fresh one if nothing was saved).  Both are gated on the display
// stereo byte (display+0x244) - the byte SetStereo flips.  Flip it off
// between frames and the last eye's history stays parked in its view state:
// mono never restores, the shared slot stays null, and SSR reads it on the
// first frame the engine does not flag as a camera cut.  Fix, at the engine's
// own call site on its own thread: the first output that begins with the byte
// off after it was on gets the one restore the engine skipped.  Covers every
// writer of the byte (menu, key, the video/cutscene mono fallback).
constexpr std::uintptr_t kOutputBeginRva = 0x067A19A0;         // FUN_1467a19a0
constexpr std::array<std::uint8_t, 27> kOutputBeginPrefix{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30,
    0x48, 0x8B, 0x1D, 0x7F, 0x7A, 0x78, 0xFE, 0x48, 0x89, 0xCF,
    0x80, 0xBB, 0x3D, 0x02, 0xB8, 0x00, 0x00};
constexpr std::uintptr_t kViewHistoryRestoreRva = 0x0E8E3580;  // FUN_14e8e3580(ctx, viewState)
constexpr std::uintptr_t kOutputIndexRva = 0x0EDB7160;         // FUN_14edb7160(display)
constexpr std::uintptr_t kDeferredCtxRva = 0x04F29430;         // DAT_144f29430
constexpr std::uintptr_t kSharedTargetsRva = 0x04F294D8;       // DAT_144f294d8
constexpr std::ptrdiff_t kViewStateArrayOffset = 0x300;        // output mgr + 0x300 + out*8
constexpr std::ptrdiff_t kTaaHistorySlot = 0x620;              // temporalAALightingTexture
using OutputBeginFn = void(__fastcall*)(std::uint8_t*);
using ViewHistoryRestoreFn = void(__fastcall*)(std::uintptr_t, void*);
using OutputIndexFn = int(__fastcall*)(void*);
OutputBeginFn g_realOutputBegin{};
std::atomic_int g_histLastGate{-1};
std::atomic_uint64_t g_histRestores{}, g_histAlreadyValid{};

void __fastcall OutputBeginDetour(std::uint8_t* mgr)
{
    auto* display = reinterpret_cast<std::uint8_t*(__fastcall*)()>(g_exeBase + kDisplayGetterRva)();
    const int gate = display && display[kStereoEnableOffset] != 0 ? 1 : 0;
    const int was = g_histLastGate.exchange(gate, std::memory_order_acq_rel);
    g_realOutputBegin(mgr);
    if (was != 1 || gate != 0 || !display || !mgr)
        return;
    const auto ctx = *reinterpret_cast<const std::uintptr_t*>(g_exeBase + kDeferredCtxRva);
    auto* shared = *reinterpret_cast<std::uint8_t* const*>(g_exeBase + kSharedTargetsRva);
    if (!ctx || !shared)
        return;
    if (*reinterpret_cast<void* const*>(shared + kTaaHistorySlot))
    {
        g_histAlreadyValid.fetch_add(1, std::memory_order_relaxed);
        Log("[HISTFIX] stereo->mono: TAA history already in place, nothing to restore");
        return;
    }
    const int out = reinterpret_cast<OutputIndexFn>(g_exeBase + kOutputIndexRva)(display);
    void* view = *reinterpret_cast<void* const*>(
        mgr + kViewStateArrayOffset + static_cast<std::ptrdiff_t>(out) * 8);
    reinterpret_cast<ViewHistoryRestoreFn>(g_exeBase + kViewHistoryRestoreRva)(ctx, view);
    const auto n = g_histRestores.fetch_add(1, std::memory_order_relaxed) + 1;
    // The render passes reach the same targets through ctx+0xB820A8 -> +8;
    // logged so the log proves the restore landed where SSR reads.
    const std::uint8_t* passTargets = nullptr;
    const auto chain = *reinterpret_cast<const std::uintptr_t*>(ctx + 0xB820A8);
    if (chain && IsReadable(reinterpret_cast<const void*>(chain + 8), sizeof(void*)))
        passTargets = *reinterpret_cast<std::uint8_t* const*>(chain + 8);
    Log("[HISTFIX] stereo->mono #%llu: restored output %d TAA history the engine left parked (view=%p) -> shared slot now %p; pass targets %p (%s)",
        static_cast<unsigned long long>(n), out, view,
        *reinterpret_cast<void* const*>(shared + kTaaHistorySlot), passTargets,
        passTargets == shared ? "same object" : "DIFFERENT object");
}

bool InstallOutputBeginHook()
{
    if (!Matches(g_exeBase + kOutputBeginRva, kOutputBeginPrefix))
    {
        Log("[HISTFIX] NOT installed: output-begin bytes differ from the analysed exe");
        return false;
    }
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("[HISTFIX] NOT installed: MinHook init %s", MH_StatusToString(init));
        return false;
    }
    auto* target = reinterpret_cast<void*>(g_exeBase + kOutputBeginRva);
    auto result = MH_CreateHook(target, reinterpret_cast<void*>(&OutputBeginDetour),
        reinterpret_cast<void**>(&g_realOutputBegin));
    if (result == MH_OK)
        result = MH_EnableHook(target);
    Log("[HISTFIX] output-begin +0x%llX %s", static_cast<unsigned long long>(kOutputBeginRva),
        result == MH_OK ? "hooked: a stereo->mono switch restores the parked TAA history"
                        : MH_StatusToString(result));
    return result == MH_OK;
}

bool WriteAbsoluteJump(CodePatch& patch, std::uintptr_t site, std::uintptr_t target)
{
    if (patch.installed)
        return true;
    std::array<std::uint8_t, 14> bytes{0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(bytes.data() + 6, &target, sizeof(target));
    constexpr std::size_t patchSize = 14;
    std::memcpy(patch.original.data(), reinterpret_cast<const void*>(site), patchSize);

    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), bytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memcpy(reinterpret_cast<void*>(site), bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), bytes.size());
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), bytes.size(), oldProtect, &ignored);
    patch.address = reinterpret_cast<void*>(site);
    patch.size = patchSize;
    patch.installed = true;
    return true;
}

void* AllocateRelayNear(std::uintptr_t site, std::uintptr_t target)
{
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto lower = site > 0x7FFF0000ull ? site - 0x7FFF0000ull : granularity;
    const auto upper = site + 0x7FFF0000ull;
    std::uintptr_t cursor = lower & ~(granularity - 1);
    while (cursor < upper)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)))
            break;
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto end = base + region.RegionSize;
        if (region.State == MEM_FREE)
        {
            const auto candidate = (base + granularity - 1) & ~(granularity - 1);
            if (candidate >= lower && candidate + 0x1000 < upper)
            {
                auto* relay = static_cast<std::uint8_t*>(VirtualAlloc(
                    reinterpret_cast<void*>(candidate), 0x1000,
                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
                if (relay)
                {
                    const std::uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
                    std::memcpy(relay, jump, sizeof(jump));
                    std::memcpy(relay + sizeof(jump), &target, sizeof(target));
                    FlushInstructionCache(GetCurrentProcess(), relay, 14);
                    return relay;
                }
            }
        }
        if (end <= cursor)
            break;
        cursor = end;
    }
    return nullptr;
}

bool WriteRelativeCallViaRelay(CodePatch& patch, std::uintptr_t site,
    std::uintptr_t target, void** relayStorage)
{
    if (patch.installed)
        return true;
    if (!relayStorage || !Matches(site, kReplayDispatchCallPrefix))
        return false;
    auto* relay = AllocateRelayNear(site, target);
    if (!relay)
        return false;
    const auto displacement64 = reinterpret_cast<std::intptr_t>(relay) -
        static_cast<std::intptr_t>(site + 5);
    if (displacement64 < INT32_MIN || displacement64 > INT32_MAX)
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::array<std::uint8_t, 6> bytes{0xE8, 0, 0, 0, 0, 0x90};
    const auto displacement = static_cast<std::int32_t>(displacement64);
    std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
    std::memcpy(patch.original.data(), reinterpret_cast<const void*>(site), bytes.size());
    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), bytes.size(),
            PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(site), bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), bytes.size());
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), bytes.size(), oldProtect, &ignored);
    patch.address = reinterpret_cast<void*>(site);
    patch.size = bytes.size();
    patch.installed = true;
    *relayStorage = relay;
    return true;
}

bool DecodeRelativeCallTarget(std::uintptr_t site, std::uintptr_t& target)
{
    if (!IsReadable(reinterpret_cast<const void*>(site), 5) ||
        *reinterpret_cast<const std::uint8_t*>(site) != 0xE8)
        return false;
    std::int32_t displacement{};
    std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1),
        sizeof(displacement));
    target = site + 5 + displacement;
    return true;
}

bool WriteValidatedProbeCallViaRelay(CodePatch& patch, std::uintptr_t site,
    std::uintptr_t expectedTarget, std::uintptr_t wrapper, void** relayStorage)
{
    if (patch.installed)
        return true;
    std::uintptr_t actualTarget{};
    if (!relayStorage || !DecodeRelativeCallTarget(site, actualTarget) ||
        actualTarget != expectedTarget)
        return false;
    auto* relay = AllocateRelayNear(site, wrapper);
    if (!relay)
        return false;
    const auto displacement64 = reinterpret_cast<std::intptr_t>(relay) -
        static_cast<std::intptr_t>(site + 5);
    if (displacement64 < INT32_MIN || displacement64 > INT32_MAX)
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::array<std::uint8_t, 5> bytes{0xE8, 0, 0, 0, 0};
    const auto displacement = static_cast<std::int32_t>(displacement64);
    std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
    std::memcpy(patch.original.data(), reinterpret_cast<const void*>(site), bytes.size());
    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), bytes.size(),
            PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(site), bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), bytes.size());
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), bytes.size(), oldProtect, &ignored);
    patch.address = reinterpret_cast<void*>(site);
    patch.size = bytes.size();
    patch.installed = true;
    *relayStorage = relay;
    return true;
}

bool WriteRelativeJumpViaRelay(CodePatch& patch, std::uintptr_t site,
    std::uintptr_t thunk, void** relayStorage)
{
    if (patch.installed)
        return true;
    if (!relayStorage || !Matches(site, kFiberOutputRestoreJmpPrefix))
        return false;
    auto* relay = static_cast<std::uint8_t*>(AllocateRelayNear(site, thunk));
    if (!relay)
        return false;
    // relay: mov rax, thunk ; jmp rax
    relay[0] = 0x48;
    relay[1] = 0xB8;
    std::memcpy(relay + 2, &thunk, sizeof(thunk));
    relay[10] = 0xFF;
    relay[11] = 0xE0;
    FlushInstructionCache(GetCurrentProcess(), relay, 12);
    const auto displacement64 = reinterpret_cast<std::intptr_t>(relay) -
        static_cast<std::intptr_t>(site + 5);
    if (displacement64 < INT32_MIN || displacement64 > INT32_MAX)
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::array<std::uint8_t, 5> bytes{0xE9, 0, 0, 0, 0};
    const auto displacement = static_cast<std::int32_t>(displacement64);
    std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
    std::memcpy(patch.original.data(), reinterpret_cast<const void*>(site), bytes.size());
    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), bytes.size(),
            PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(site), bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), bytes.size());
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), bytes.size(), oldProtect, &ignored);
    patch.address = reinterpret_cast<void*>(site);
    patch.size = bytes.size();
    patch.installed = true;
    *relayStorage = relay;
    return true;
}

bool WriteReplayDrawTailViaRelay(CodePatch& patch, std::uintptr_t site,
    const std::array<std::uint8_t, 8>& expected, std::uintptr_t policy,
    std::uintptr_t continuation, void** relayStorage)
{
    if (patch.installed)
        return true;
    if (!relayStorage || !Matches(site, expected))
        return false;
    auto* relay = static_cast<std::uint8_t*>(AllocateRelayNear(site, policy));
    if (!relay)
        return false;

    // The original three-byte Draw call is immediately followed by an unconditional five-byte
    // jump. Replace the complete eight-byte tail with a jump to this relay, call the policy with
    // the already-prepared rcx/edx/r8 arguments, then jump to the original continuation.
    const std::uint8_t relayPrefix[] = {
        0x48, 0x83, 0xEC, 0x20,                         // sub rsp, 20h
        0x48, 0xB8,                                     // mov rax, policy
    };
    std::memcpy(relay, relayPrefix, sizeof(relayPrefix));
    std::memcpy(relay + 6, &policy, sizeof(policy));
    const std::uint8_t callAndRestore[] = {
        0xFF, 0xD0,                                     // call rax
        0x48, 0x83, 0xC4, 0x20,                         // add rsp, 20h
        0x48, 0xB8,                                     // mov rax, continuation
    };
    std::memcpy(relay + 14, callAndRestore, sizeof(callAndRestore));
    std::memcpy(relay + 22, &continuation, sizeof(continuation));
    const std::uint8_t jumpRax[] = {0xFF, 0xE0};
    std::memcpy(relay + 30, jumpRax, sizeof(jumpRax));
    FlushInstructionCache(GetCurrentProcess(), relay, 32);

    const auto displacement64 = reinterpret_cast<std::intptr_t>(relay) -
        static_cast<std::intptr_t>(site + 5);
    if (displacement64 < INT32_MIN || displacement64 > INT32_MAX)
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::array<std::uint8_t, 8> bytes{0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const auto displacement = static_cast<std::int32_t>(displacement64);
    std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
    std::memcpy(patch.original.data(), reinterpret_cast<const void*>(site), bytes.size());
    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), bytes.size(),
            PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(site), bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), bytes.size());
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), bytes.size(), oldProtect, &ignored);
    patch.address = reinterpret_cast<void*>(site);
    patch.size = bytes.size();
    patch.installed = true;
    *relayStorage = relay;
    return true;
}

bool WriteReplayMapViaRelay(CodePatch& patch, std::uintptr_t site,
    std::uintptr_t policy, void** relayStorage)
{
    if (patch.installed)
        return true;
    if (!relayStorage || !Matches(site, kReplayMapCallPrefix))
        return false;
    auto* relay = static_cast<std::uint8_t*>(AllocateRelayNear(site, policy));
    if (!relay)
        return false;

    // Preserve the original fifth and sixth Map arguments while creating fresh shadow space.
    // The relay also executes the overwritten mapped-data load before returning to +0x2DB0DA0.
    const std::uint8_t relayPrefix[] = {
        0x48, 0x83, 0xEC, 0x40,
        0x48, 0x8B, 0x44, 0x24, 0x60,
        0x48, 0x89, 0x44, 0x24, 0x20,
        0x48, 0x8B, 0x44, 0x24, 0x68,
        0x48, 0x89, 0x44, 0x24, 0x28,
        0x48, 0xB8,
    };
    std::memcpy(relay, relayPrefix, sizeof(relayPrefix));
    std::memcpy(relay + 26, &policy, sizeof(policy));
    const std::uint8_t callAndContinuePrefix[] = {
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x40,
        0x48, 0x8B, 0x4C, 0x24, 0x50,
        0x48, 0xB8,
    };
    std::memcpy(relay + 34, callAndContinuePrefix, sizeof(callAndContinuePrefix));
    const auto continuation = site + kReplayMapCallPrefix.size();
    std::memcpy(relay + 47, &continuation, sizeof(continuation));
    const std::uint8_t jumpRax[] = {0xFF, 0xE0};
    std::memcpy(relay + 55, jumpRax, sizeof(jumpRax));
    FlushInstructionCache(GetCurrentProcess(), relay, 57);

    const auto displacement64 = reinterpret_cast<std::intptr_t>(relay) -
        static_cast<std::intptr_t>(site + 5);
    if (displacement64 < INT32_MIN || displacement64 > INT32_MAX)
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::array<std::uint8_t, 8> bytes{0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const auto displacement = static_cast<std::int32_t>(displacement64);
    std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
    std::memcpy(patch.original.data(), reinterpret_cast<const void*>(site), bytes.size());
    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), bytes.size(),
            PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(site), bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), bytes.size());
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), bytes.size(), oldProtect, &ignored);
    patch.address = reinterpret_cast<void*>(site);
    patch.size = bytes.size();
    patch.installed = true;
    *relayStorage = relay;
    return true;
}

bool WritePointerPatch(CodePatch& patch, std::uintptr_t site, const void* target, void** original)
{
    if (patch.installed)
        return true;
    if (!target || !original || !IsReadable(reinterpret_cast<const void*>(site), sizeof(void*)))
        return false;
    std::memcpy(patch.original.data(), reinterpret_cast<const void*>(site), sizeof(void*));
    std::memcpy(original, reinterpret_cast<const void*>(site), sizeof(void*));
    if (!*original)
        return false;
    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    std::memcpy(reinterpret_cast<void*>(site), &target, sizeof(target));
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), sizeof(void*), oldProtect, &ignored);
    patch.address = reinterpret_cast<void*>(site);
    patch.size = sizeof(void*);
    patch.installed = true;
    return true;
}

bool WriteTrampolineJump(CodePatch& patch, std::uintptr_t site, std::size_t stolenSize,
    std::uintptr_t target, void** original)
{
    if (patch.installed)
        return true;
    if (!original || stolenSize < 14 || stolenSize > patch.original.size())
        return false;

    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, stolenSize + 14,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!trampoline)
        return false;
    std::memcpy(trampoline, reinterpret_cast<const void*>(site), stolenSize);
    std::uint8_t returnJump[14] = {0xFF, 0x25, 0, 0, 0, 0};
    const std::uintptr_t returnAddress = site + stolenSize;
    std::memcpy(returnJump + 6, &returnAddress, sizeof(returnAddress));
    std::memcpy(trampoline + stolenSize, returnJump, sizeof(returnJump));
    FlushInstructionCache(GetCurrentProcess(), trampoline, stolenSize + sizeof(returnJump));

    std::memcpy(patch.original.data(), reinterpret_cast<const void*>(site), stolenSize);
    std::uint8_t entryJump[14] = {0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(entryJump + 6, &target, sizeof(target));
    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), stolenSize, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(site), entryJump, sizeof(entryJump));
    for (std::size_t index = sizeof(entryJump); index < stolenSize; ++index)
        *reinterpret_cast<std::uint8_t*>(site + index) = 0x90;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), stolenSize);
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), stolenSize, oldProtect, &ignored);
    patch.address = reinterpret_cast<void*>(site);
    patch.size = stolenSize;
    patch.installed = true;
    *original = trampoline;
    return true;
}

void RestoreCodePatch(CodePatch& patch)
{
    if (!patch.installed || !patch.address)
        return;
    DWORD oldProtect{};
    if (VirtualProtect(patch.address, patch.size, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        std::memcpy(patch.address, patch.original.data(), patch.size);
        FlushInstructionCache(GetCurrentProcess(), patch.address, patch.size);
        DWORD ignored{};
        VirtualProtect(patch.address, patch.size, oldProtect, &ignored);
    }
    patch.installed = false;
}

bool PatchVtable(void** table, std::size_t index, void* replacement, void** original)
{
    if (!table || !replacement || !original)
        return false;
    void** slot = table + index;
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

std::uint32_t BitsPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 128;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 96;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return 64;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return 32;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
        return 32;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM:
        return 16;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
        return 8;
    default:
        return 0;
    }
}

std::uint64_t EstimateTextureBytes(const D3D11_TEXTURE2D_DESC& desc)
{
    const bool bc1 = desc.Format == DXGI_FORMAT_BC1_TYPELESS ||
        desc.Format == DXGI_FORMAT_BC1_UNORM || desc.Format == DXGI_FORMAT_BC1_UNORM_SRGB;
    const bool bcWide = desc.Format == DXGI_FORMAT_BC2_TYPELESS ||
        desc.Format == DXGI_FORMAT_BC2_UNORM || desc.Format == DXGI_FORMAT_BC2_UNORM_SRGB ||
        desc.Format == DXGI_FORMAT_BC3_TYPELESS || desc.Format == DXGI_FORMAT_BC3_UNORM ||
        desc.Format == DXGI_FORMAT_BC3_UNORM_SRGB || desc.Format == DXGI_FORMAT_BC5_TYPELESS ||
        desc.Format == DXGI_FORMAT_BC5_UNORM || desc.Format == DXGI_FORMAT_BC5_SNORM ||
        desc.Format == DXGI_FORMAT_BC6H_TYPELESS || desc.Format == DXGI_FORMAT_BC6H_UF16 ||
        desc.Format == DXGI_FORMAT_BC6H_SF16 || desc.Format == DXGI_FORMAT_BC7_TYPELESS ||
        desc.Format == DXGI_FORMAT_BC7_UNORM || desc.Format == DXGI_FORMAT_BC7_UNORM_SRGB;
    const std::uint32_t bits = BitsPerPixel(desc.Format);
    if (bits == 0 && !bc1 && !bcWide)
        return 0;
    std::uint64_t total{};
    std::uint32_t width = desc.Width;
    std::uint32_t height = desc.Height;
    const std::uint32_t levels = desc.MipLevels == 0 ? 1 : desc.MipLevels;
    for (std::uint32_t level = 0; level < levels; ++level)
    {
        if (bc1 || bcWide)
            total += static_cast<std::uint64_t>((width + 3) / 4) * ((height + 3) / 4) *
                (bc1 ? 8 : 16);
        else
            total += (static_cast<std::uint64_t>(width) * height * bits + 7) / 8;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    return total * (desc.ArraySize ? desc.ArraySize : 1) *
        (desc.SampleDesc.Count ? desc.SampleDesc.Count : 1);
}

void AttachLeakSentinel(ID3D11DeviceChild* resource, LeakResourceKind kind, std::uint64_t bytes,
    bool depth1024 = false)
{
    if (!resource)
        return;
    auto* sentinel = new (std::nothrow) LeakResourceSentinel(kind, bytes, depth1024);
    if (!sentinel)
        return;
    if (kind == LeakResourceKind::Buffer)
    {
        g_buffersCreated.fetch_add(1, std::memory_order_relaxed);
        g_bufferBytesCreated.fetch_add(bytes, std::memory_order_relaxed);
    }
    else
    {
        g_texturesCreated.fetch_add(1, std::memory_order_relaxed);
        g_textureBytesCreated.fetch_add(bytes, std::memory_order_relaxed);
    }
    resource->SetPrivateDataInterface(kLeakSentinelGuid, sentinel);
    sentinel->Release();
}

HRESULT STDMETHODCALLTYPE HookCreateBuffer(ID3D11Device* device, const D3D11_BUFFER_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** buffer)
{
    const HRESULT result = g_realCreateBuffer
        ? g_realCreateBuffer(device, desc, data, buffer) : E_FAIL;
    if (SUCCEEDED(result) && buffer && *buffer)
        AttachLeakSentinel(*buffer, LeakResourceKind::Buffer, desc ? desc->ByteWidth : 0);
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** texture)
{
    const HRESULT result = g_realCreateTexture2D
        ? g_realCreateTexture2D(device, desc, data, texture) : E_FAIL;
    if (SUCCEEDED(result) && texture && *texture)
    {
        AttachLeakSentinel(*texture, LeakResourceKind::Texture,
            desc ? EstimateTextureBytes(*desc) : 0,
            desc && desc->Width == 1024 && desc->Height == 1024 &&
                desc->Format == DXGI_FORMAT_R32_TYPELESS);
        if (desc && g_stereoEnabled.load(std::memory_order_acquire))
        {
            const auto sample = g_stereoTextureSamples.fetch_add(1, std::memory_order_relaxed) + 1;
            if (sample <= 48)
            {
                const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
                const auto rva = caller >= g_exeBase ? caller - g_exeBase : 0;
                Log("[TEXLEAK] #%u caller=%p rva=0x%llX %ux%u fmt=%u mips=%u array=%u "
                    "samples=%u usage=%u bind=0x%X cpu=0x%X misc=0x%X est=%lluKB",
                    sample, reinterpret_cast<void*>(caller),
                    static_cast<unsigned long long>(rva), desc->Width, desc->Height,
                    static_cast<unsigned>(desc->Format), desc->MipLevels, desc->ArraySize,
                    desc->SampleDesc.Count, static_cast<unsigned>(desc->Usage), desc->BindFlags,
                    desc->CPUAccessFlags, desc->MiscFlags,
                    static_cast<unsigned long long>(EstimateTextureBytes(*desc) / 1024));

                if (desc->Width == 1024 && desc->Height == 1024 &&
                    desc->Format == DXGI_FORMAT_R32_TYPELESS)
                {
                    const auto stackSample = g_depthStackSamples.fetch_add(1,
                        std::memory_order_relaxed) + 1;
                    if (stackSample <= 4)
                    {
                        void* frames[24]{};
                        const USHORT count = CaptureStackBackTrace(0, _countof(frames), frames, nullptr);
                        char stackLine[1792]{};
                        StringCchPrintfA(stackLine, _countof(stackLine),
                            "[TEXSTACK] #%u frames=%u exe-rvas:", stackSample, count);
                        for (USHORT frame = 0; frame < count; ++frame)
                        {
                            const auto address = reinterpret_cast<std::uintptr_t>(frames[frame]);
                            if (address < g_exeBase || address >= g_exeBase + kRetailImageSize)
                                continue;
                            char suffix[48]{};
                            StringCchPrintfA(suffix, _countof(suffix), " 0x%llX",
                                static_cast<unsigned long long>(address - g_exeBase));
                            StringCchCatA(stackLine, _countof(stackLine), suffix);
                        }
                        WriteLogLine(stackLine);
                    }
                }
            }
        }
    }
    return result;
}

void InstallResourceProbe(ID3D11Device* device)
{
    if (!device)
        return;
    bool expected = false;
    if (!g_resourceProbeInstalled.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    void** table = *reinterpret_cast<void***>(device);
    const bool bufferOk = PatchVtable(table, 3, reinterpret_cast<void*>(&HookCreateBuffer),
        reinterpret_cast<void**>(&g_realCreateBuffer));
    const bool textureOk = PatchVtable(table, 5, reinterpret_cast<void*>(&HookCreateTexture2D),
        reinterpret_cast<void**>(&g_realCreateTexture2D));

    IDXGIDevice* dxgiDevice{};
    IDXGIAdapter* adapter{};
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
            reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice)
    {
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter)
            adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                reinterpret_cast<void**>(&g_leakAdapter));
    }
    if (adapter)
        adapter->Release();
    if (dxgiDevice)
        dxgiDevice->Release();

    Log("[LEAK] resource probe installed buffer=%d texture2d=%d gpuTelemetry=%d",
        bufferOk ? 1 : 0, textureOk ? 1 : 0, g_leakAdapter ? 1 : 0);
}

// [VTALIAS] Meta's runtime is known to replace D3D11 context vtables when its
// XR session starts. A replacement that was COPIED
// from a hooked table still routes into the mod's hooks, but with an unknown
// table the originals lookup would fail and every hooked call would return an
// error. Recognise the copy (its untouched slots 0-6 and 8-11 match exactly)
// and adopt it as an alias of the hooked table, so the mod's hooks keep calling the
// real functions. Anything that is not such a copy still returns null.
ContextVtableHook* AliasCopiedContextVtable(void** table)
{
    static constexpr int kProbeSlots[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11};
    for (auto& hook : g_contextVtableHooks)
    {
        void** known = hook.table.load(std::memory_order_acquire);
        if (!known || hook.alias.load(std::memory_order_acquire))
            continue;
        bool same = true;
        __try
        {
            for (int slot : kProbeSlots)
                if (table[slot] != known[slot]) { same = false; break; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
        if (!same)
            continue;
        void** expected = nullptr;
        if (hook.alias.compare_exchange_strong(expected, table, std::memory_order_acq_rel))
            Log("[VTALIAS] context vtable %p replaced by copy %p; hooks follow it", known, table);
        return hook.alias.load(std::memory_order_acquire) == table ? &hook : nullptr;
    }
    return nullptr;
}

ContextVtableHook* FindContextVtableHook(void** table)
{
    if (!table)
        return nullptr;
    for (auto& hook : g_contextVtableHooks)
    {
        if (hook.table.load(std::memory_order_acquire) == table ||
            hook.alias.load(std::memory_order_acquire) == table)
            return &hook;
    }
    return AliasCopiedContextVtable(table);
}

PoseBufferSample* FindPoseBuffer(ID3D11Resource* resource, std::uintptr_t caller,
    bool allowCreate)
{
    if (!resource)
        return nullptr;
    const auto key = reinterpret_cast<std::uintptr_t>(resource);
    const std::size_t start = ((key >> 4) ^ (key >> 17)) & (kPoseBufferSampleCount - 1);
    for (std::size_t probe = 0; probe < 64; ++probe)
    {
        auto& sample = g_poseBuffers[(start + probe) & (kPoseBufferSampleCount - 1)];
        auto existing = sample.resource.load(std::memory_order_acquire);
        if (existing == key)
        {
            if (caller >= g_exeBase && caller < g_exeBase + kRetailImageSize)
                sample.callerRva.store(caller - g_exeBase, std::memory_order_relaxed);
            return &sample;
        }
        if (existing != 0 || !allowCreate)
            continue;

        D3D11_RESOURCE_DIMENSION dimension{};
        resource->GetType(&dimension);
        if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
            return nullptr;
        if (!sample.resource.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        D3D11_BUFFER_DESC desc{};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        sample.byteWidth = desc.ByteWidth;
        sample.bindFlags = desc.BindFlags;
        if (caller >= g_exeBase && caller < g_exeBase + kRetailImageSize)
            sample.callerRva.store(caller - g_exeBase, std::memory_order_relaxed);
        g_poseBuffersObserved.fetch_add(1, std::memory_order_relaxed);
        return &sample;
    }
    return nullptr;
}

std::uint64_t HashPoseBuffer(const void* data, std::uint32_t size)
{
    if (!data || size == 0)
        return 0;
    constexpr std::uint64_t kOffset = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffset;
    __try
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        const std::uint32_t samples = size < 256 ? size : 64;
        for (std::uint32_t sample = 0; sample < samples; ++sample)
        {
            const std::uint32_t offset = samples == size ? sample :
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(sample) * (size - 1) /
                    (samples - 1));
            hash = (hash ^ bytes[offset]) * kPrime;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    return hash;
}

// NaN census: temporal-feedback poisoning (NaN never heals in a blend loop) is
// one way lighting corrupts.  Scan every observed buffer upload
// for NaN floats, bucketed by mode, tallied per producer RVA.
struct NanWriterCell
{
    std::atomic_uintptr_t rva{};
    std::atomic_uint64_t count{};
};
constexpr std::size_t kNanWriterSlots = 128;
std::array<NanWriterCell, kNanWriterSlots> g_nanWriters{};
std::array<std::atomic_uint64_t, 4> g_nanPayloads{};
std::array<std::atomic_uint64_t, 4> g_scannedPayloads{};
std::atomic_uint64_t g_nanShots{};

void ScanUploadForNan(PoseBufferSample* sample, const void* data, std::uint32_t size)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    // Constant buffers only: vertex/index/streaming buffers carry packed
    // non-float data whose bit patterns alias NaN (proved: 30%
    // false-positive rate on 32MB bind=0x1 uploads in clean mono).
    if ((sample->bindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0 || size > 0x10000)
        return;
    std::uint32_t firstNanOffset = 0xFFFFFFFFu;
    __try
    {
        const auto* words = static_cast<const std::uint32_t*>(data);
        const std::uint32_t count = size / 4 < 1024 ? size / 4 : 1024;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto bits = words[i];
            if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0)
            {
                firstNanOffset = i * 4;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    const bool stereo = g_stereoEnabled.load(std::memory_order_acquire);
    const std::uint32_t mode = stereo ? (ReadTrueOutput() == 1 ? 2u : 1u)
        : (g_stereoEverEnabled.load(std::memory_order_acquire) ? 3u : 0u);
    g_scannedPayloads[mode].fetch_add(1, std::memory_order_relaxed);
    if (firstNanOffset == 0xFFFFFFFFu)
        return;
    g_nanPayloads[mode].fetch_add(1, std::memory_order_relaxed);
    std::uintptr_t writer = sample->updateCallerRva.load(std::memory_order_relaxed);
    if (!writer)
        writer = sample->producerUnmapCallerRva.load(std::memory_order_relaxed);
    if (!writer)
        writer = sample->producerMapCallerRva.load(std::memory_order_relaxed);
    if (writer)
    {
        auto slot = static_cast<std::size_t>((writer ^ (writer >> 9)) & (kNanWriterSlots - 1));
        for (std::size_t probe = 0; probe < kNanWriterSlots; ++probe)
        {
            auto& cell = g_nanWriters[(slot + probe) & (kNanWriterSlots - 1)];
            auto current = cell.rva.load(std::memory_order_acquire);
            if (current == writer)
            {
                cell.count.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            std::uintptr_t expected = 0;
            if (current == 0 && cell.rva.compare_exchange_strong(expected, writer,
                    std::memory_order_acq_rel))
            {
                cell.count.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    const auto shot = g_nanShots.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shot <= 6)
    {
        static const char* const kModeNames[4] = {"cleanmono", "o0", "o1", "postmono"};
        Log("[NANSCAN] #%llu mode=%s size=%u firstNanAt=%u writer=+0x%llX bind=0x%X",
            static_cast<unsigned long long>(shot), kModeNames[mode], size, firstNanOffset,
            static_cast<unsigned long long>(writer), sample->bindFlags);
    }
}

void RecordPoseBuffer(PoseBufferSample* sample, const void* data, std::uint32_t size)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!sample)
        return;
    ScanUploadForNan(sample, data, size);
    const auto hash = HashPoseBuffer(data, size);
    if (!hash)
        return;
    const auto previous = sample->lastHash.exchange(hash, std::memory_order_acq_rel);
    sample->writes.fetch_add(1, std::memory_order_relaxed);
    if (previous != 0 && previous != hash)
        sample->changes.fetch_add(1, std::memory_order_relaxed);
}

void LogPoseSourceRegion(std::size_t rank, std::uintptr_t source)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!source)
    {
        Log("[POSESRC] rank=%zu source=null", rank);
        return;
    }

    MEMORY_BASIC_INFORMATION sourceInfo{};
    if (!VirtualQuery(reinterpret_cast<const void*>(source), &sourceInfo,
            sizeof(sourceInfo)) || sourceInfo.State != MEM_COMMIT ||
        (sourceInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
    {
        Log("[POSESRC] rank=%zu source=%p unavailable", rank,
            reinterpret_cast<void*>(source));
        return;
    }

    const auto allocationBase = reinterpret_cast<std::uintptr_t>(sourceInfo.AllocationBase);
    const auto regionBase = reinterpret_cast<std::uintptr_t>(sourceInfo.BaseAddress);
    Log("[POSESRC] rank=%zu source=%p alloc=%p allocOffset=0x%llX region=%p "
        "regionOffset=0x%llX regionSize=0x%llX protect=0x%lX type=0x%lX",
        rank, reinterpret_cast<void*>(source), sourceInfo.AllocationBase,
        static_cast<unsigned long long>(source - allocationBase), sourceInfo.BaseAddress,
        static_cast<unsigned long long>(source - regionBase),
        static_cast<unsigned long long>(sourceInfo.RegionSize), sourceInfo.Protect,
        sourceInfo.Type);

    // Pose command data is commonly embedded after an owner/header. Report a few nearby
    // pointer-sized fields that lead to committed memory, plus direct code/vtable RVAs.
    // This is telemetry-only and runs once per ranked stale source per second.
    std::size_t reported{};
    for (std::size_t distance = sizeof(std::uintptr_t);
         distance <= 0x80 && distance <= source && reported < 3;
         distance += sizeof(std::uintptr_t))
    {
        const auto field = source - distance;
        if (!IsReadable(reinterpret_cast<const void*>(field), sizeof(std::uintptr_t)))
            continue;
        std::uintptr_t value{};
        __try
        {
            value = *reinterpret_cast<const std::uintptr_t*>(field);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            continue;
        }
        if ((value & (sizeof(std::uintptr_t) - 1)) != 0 ||
            !IsReadable(reinterpret_cast<const void*>(value), sizeof(std::uintptr_t)))
            continue;

        MEMORY_BASIC_INFORMATION targetInfo{};
        if (!VirtualQuery(reinterpret_cast<const void*>(value), &targetInfo,
                sizeof(targetInfo)))
            continue;
        std::uintptr_t first{};
        __try
        {
            first = *reinterpret_cast<const std::uintptr_t*>(value);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            first = 0;
        }
        const auto directRva = value >= g_exeBase && value < g_exeBase + kRetailImageSize
            ? value - g_exeBase : 0;
        const auto vtableRva = first >= g_exeBase && first < g_exeBase + kRetailImageSize
            ? first - g_exeBase : 0;
        Log("[POSESRC] rank=%zu pre=-0x%zX value=%p targetAlloc=%p "
            "targetRegion=%p direct=+0x%llX vtable=+0x%llX",
            rank, distance, reinterpret_cast<void*>(value), targetInfo.AllocationBase,
            targetInfo.BaseAddress, static_cast<unsigned long long>(directRva),
            static_cast<unsigned long long>(vtableRva));
        ++reported;
    }
    if (reported == 0)
        Log("[POSESRC] rank=%zu nearbyPointers=0", rank);
}

using ReplayCopyFn = void*(__cdecl*)(void*, const void*, std::size_t);
ReplayCopyFn g_realReplayCopy{};

void RecordCopyWriter(std::uintptr_t destination, std::uint32_t size,
    std::uintptr_t callerRva)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const std::size_t start = ((destination >> 4) ^ (destination >> 19)) &
        (kCopyWriterSampleCount - 1);
    for (std::size_t probe = 0; probe < 16; ++probe)
    {
        auto& slot = g_copyWriters[(start + probe) & (kCopyWriterSampleCount - 1)];
        auto existing = slot.destination.load(std::memory_order_acquire);
        if (existing == 0 && !slot.destination.compare_exchange_strong(existing, destination,
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        if (existing != 0 && existing != destination)
            continue;
        slot.size.store(size, std::memory_order_relaxed);
        slot.callerRva.store(callerRva, std::memory_order_release);
        return;
    }
    auto& replacement = g_copyWriters[start];
    replacement.callerRva.store(0, std::memory_order_release);
    replacement.size.store(size, std::memory_order_relaxed);
    replacement.destination.store(destination, std::memory_order_release);
    replacement.callerRva.store(callerRva, std::memory_order_release);
}

std::uintptr_t FindCopyWriter(std::uintptr_t destination)
{
    const std::size_t start = ((destination >> 4) ^ (destination >> 19)) &
        (kCopyWriterSampleCount - 1);
    for (std::size_t probe = 0; probe < 16; ++probe)
    {
        const auto& slot = g_copyWriters[(start + probe) & (kCopyWriterSampleCount - 1)];
        const auto existing = slot.destination.load(std::memory_order_acquire);
        if (existing == destination)
            return slot.callerRva.load(std::memory_order_acquire);
        if (existing == 0)
            break;
    }
    return 0;
}

extern "C" HRESULT STDMETHODCALLTYPE RetailReplayMapPolicy(ID3D11DeviceContext* context,
    ID3D11Resource* resource, UINT subresource, D3D11_MAP mapType, UINT mapFlags,
    D3D11_MAPPED_SUBRESOURCE* mapped)
{
    g_mappedPoseSample = nullptr;
    if (!context)
        return E_INVALIDARG;
    auto** table = *reinterpret_cast<void***>(context);
    const auto map = table ? reinterpret_cast<MapFn>(table[14]) : nullptr;
    const HRESULT result = map
        ? map(context, resource, subresource, mapType, mapFlags, mapped) : E_FAIL;
    if (SUCCEEDED(result) && mapped && mapType != D3D11_MAP_READ)
    {
        auto* sample = FindPoseBuffer(resource, g_exeBase + kReplayMapCallRva, true);
        if (sample)
        {
            sample->mappedData.store(reinterpret_cast<std::uintptr_t>(mapped->pData),
                std::memory_order_release);
            sample->lastMapType.store(static_cast<std::uint32_t>(mapType),
                std::memory_order_relaxed);
            g_mappedPoseSample = sample;
        }
    }
    return result;
}

void* __cdecl HookReplayCopy(void* destination, const void* source, std::size_t size)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (caller == g_exeBase + kReplayCopyReturnRva &&
        g_mappedPoseSample && source && destination && size != 0 &&
        g_mappedPoseSample->mappedData.load(std::memory_order_acquire) ==
            reinterpret_cast<std::uintptr_t>(destination))
    {
        const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source);
        g_mappedPoseSample->replaySource.store(sourceAddress,
            std::memory_order_release);
        g_mappedPoseSample->sourceWriterRva.store(FindCopyWriter(sourceAddress),
            std::memory_order_release);
        const auto copiedSize = static_cast<std::uint32_t>(
            size < g_mappedPoseSample->byteWidth ? size : g_mappedPoseSample->byteWidth);
        RecordPoseBuffer(g_mappedPoseSample, source, copiedSize);
    }
    else if (caller >= g_exeBase && caller < g_exeBase + kRetailImageSize &&
        destination && size != 0 && size <= 4096)
        RecordCopyWriter(reinterpret_cast<std::uintptr_t>(destination),
            static_cast<std::uint32_t>(size), caller - g_exeBase);
    return g_realReplayCopy ? g_realReplayCopy(destination, source, size) : destination;
}

void ProbeMainCameraPayload(float* payload, void* resource)
{
    __try
    {
        for (int row = 0; row < 4; ++row)
        {
            const float wTerm = payload[row * 4 + 3];
            const float forward = payload[32 + row * 4 + 2];
            float magnitude = forward < 0.0f ? -forward : forward;
            if (magnitude < 1.0f)
                magnitude = 1.0f;
            float error = wTerm + forward;
            if (error < 0.0f)
                error = -error;
            if (error > 1e-3f * magnitude)
            {
                g_o0SkewFiltered.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        const float skew = payload[34] * payload[0] + payload[38] * payload[4] +
            payload[42] * payload[8];
        if (!(skew == skew) || skew < -0.6f || skew > 0.6f)
            return;
        const float skewMagnitude = skew < 0.0f ? -skew : skew;
        const auto present = g_presentCount.load(std::memory_order_relaxed);
        const auto mode = g_o0SkewProbeMode.load(std::memory_order_relaxed);
        if (skewMagnitude > 1e-3f)
        {
            if (TryAcquireSRWLockExclusive(&g_mainCamSkewRef.lock))
            {
                g_mainCamSkewRef.skew = skew;
                g_mainCamSkewRef.position[0] = payload[48];
                g_mainCamSkewRef.position[1] = payload[49];
                g_mainCamSkewRef.position[2] = payload[50];
                g_mainCamSkewRef.present = present;
                ReleaseSRWLockExclusive(&g_mainCamSkewRef.lock);
                g_o0SkewMeasured.fetch_add(1, std::memory_order_relaxed);
            }
            if (mode == kO0SkewModeSway)
            {
                // A skewed perspective write IS the main camera; sway it
                // directly.  Triangle wave, period 90 presents, +/-0.15 NDC.
                const auto phase = static_cast<float>(present % 90u);
                const float wave = phase < 45.0f
                    ? phase / 22.5f - 1.0f : 3.0f - phase / 22.5f;
                const float sway = 0.15f * wave;
                for (int row = 0; row < 4; ++row)
                    payload[row * 4] += sway * payload[32 + row * 4 + 2];
                alignas(16) float swayInverse[16]{};
                if (InvertMatrix4x4(payload, swayInverse))
                {
                    for (int index = 0; index < 16; ++index)
                        payload[16 + index] = swayInverse[index];
                }
                else
                    g_o0SkewInverseFailed.fetch_add(1,
                        std::memory_order_relaxed);
                g_o0SkewSwayed.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        if (skewMagnitude > 1e-4f || mode == kO0SkewModeOff)
            return;
        float referenceSkew = 0.0f;
        float referencePosition[3]{};
        std::uint64_t referencePresent = 0;
        AcquireSRWLockShared(&g_mainCamSkewRef.lock);
        referenceSkew = g_mainCamSkewRef.skew;
        referencePosition[0] = g_mainCamSkewRef.position[0];
        referencePosition[1] = g_mainCamSkewRef.position[1];
        referencePosition[2] = g_mainCamSkewRef.position[2];
        referencePresent = g_mainCamSkewRef.present;
        ReleaseSRWLockShared(&g_mainCamSkewRef.lock);
        if (referenceSkew == 0.0f || present < referencePresent ||
            present - referencePresent > 30)
            return;
        for (int axis = 0; axis < 3; ++axis)
        {
            float delta = payload[48 + axis] - referencePosition[axis];
            if (delta < 0.0f)
                delta = -delta;
            if (delta > 2.0f)
                return;
        }
        if (mode == kO0SkewModeSway)
        {
            const auto phase = static_cast<float>(present % 90u);
            const float wave = phase < 45.0f
                ? phase / 22.5f - 1.0f : 3.0f - phase / 22.5f;
            const float sway = 0.15f * wave;
            for (int row = 0; row < 4; ++row)
                payload[row * 4] += sway * payload[32 + row * 4 + 2];
            g_o0SkewSwayed.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            for (int row = 0; row < 4; ++row)
                payload[row * 4] -= referenceSkew * payload[32 + row * 4 + 2];
            g_o0SkewApplied.fetch_add(1, std::memory_order_relaxed);
        }
        alignas(16) float inverse[16]{};
        if (InvertMatrix4x4(payload, inverse))
        {
            for (int index = 0; index < 16; ++index)
                payload[16 + index] = inverse[index];
        }
        else
            g_o0SkewInverseFailed.fetch_add(1, std::memory_order_relaxed);
        for (auto& slot : g_o0SkewPatchedResources)
        {
            std::uintptr_t expected = 0;
            const auto value = reinterpret_cast<std::uintptr_t>(resource);
            if (slot.compare_exchange_strong(expected, value,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                Log("[O0SKEW] patching resource=%p skew=%.6f "
                    "pos=(%.2f,%.2f,%.2f) present=%llu",
                    resource, referenceSkew, payload[48], payload[49],
                    payload[50], static_cast<unsigned long long>(present));
                break;
            }
            if (expected == value)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void SwayProjectionBlocks(float* dataFloats, std::uint32_t byteWidth,
    void* resource, std::uint32_t slotMask)
{
    __try
    {
        const auto present = g_presentCount.load(std::memory_order_relaxed);
        const auto phase = static_cast<float>(present % 90u);
        const float wave = phase < 45.0f
            ? phase / 22.5f - 1.0f : 3.0f - phase / 22.5f;
        const float sway = 0.15f * wave;
        constexpr float kDepthScale = 1.000017f;
        constexpr float kDepthNear = -0.200003f;
        int patchedBlocks = 0;
        for (std::uint32_t offset = 0; offset + 64 <= byteWidth; offset += 64)
        {
            float* block = dataFloats + offset / 4;
            bool finite = true;
            for (int index = 0; index < 16 && finite; ++index)
            {
                const float value = block[index];
                if (!(value == value) || !(value - value == 0.0f))
                    finite = false;
            }
            if (!finite)
                continue;
            bool match = true;
            for (int row = 0; row < 3 && match; ++row)
            {
                const float lhs = block[row * 4 + 2];
                const float expected = kDepthScale * block[row * 4 + 3];
                float error = lhs - expected;
                if (error < 0.0f)
                    error = -error;
                float scale = lhs < 0.0f ? -lhs : lhs;
                if (scale < 1.0f)
                    scale = 1.0f;
                if (error > 1e-3f * scale)
                    match = false;
            }
            if (match)
            {
                float recoveredNear =
                    block[14] - kDepthScale * block[15] - kDepthNear;
                if (recoveredNear < 0.0f)
                    recoveredNear = -recoveredNear;
                if (recoveredNear > 1e-2f)
                    match = false;
            }
            if (!match)
                continue;
            for (int row = 0; row < 4; ++row)
                block[row * 4] -= sway * block[row * 4 + 3];
            ++patchedBlocks;
        }
        if (!patchedBlocks)
            return;
        g_objSwayBlocks.fetch_add(static_cast<std::uint64_t>(patchedBlocks),
            std::memory_order_relaxed);
        g_objSwayWrites.fetch_add(1, std::memory_order_relaxed);
        for (auto& slot : g_objSwayResources)
        {
            std::uintptr_t expected = 0;
            const auto value = reinterpret_cast<std::uintptr_t>(resource);
            if (slot.compare_exchange_strong(expected, value,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                Log("[OBJSWAY] resource=%p bytes=%u slots=0x%X blocks=%d "
                    "present=%llu",
                    resource, byteWidth, slotMask, patchedBlocks,
                    static_cast<unsigned long long>(present));
                break;
            }
            if (expected == value)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void UniformRecenterBlocks(float* dataFloats, std::uint32_t byteWidth,
    void* resource)
{
    __try
    {
        // Skew directly from engine state: P[2][0] = baseline/scale, proven
        // exact against the captured payloads (6/115.117699 = 0.052121).
        // The write-phase TLS tag flaps per write (left-tagged injection
        // jitters objects and flashes lights without moving the left
        // camera), so no per-half classification is
        // attempted.  Instead EVERY projection-composed block, both phases,
        // all buffer sizes, shifts by dP[2][0] = -skew/2: the engine-skewed
        // right matrices become +skew/2 and the mono left matrices become
        // -skew/2 - symmetric separation by construction, deterministic per
        // write, no flicker.
        float referenceSkew = 0.0f;
        if (g_stereoBaselineAddress && g_stereoScaleAddress)
        {
            const float scale = *g_stereoScaleAddress;
            if (scale > 1e-3f || scale < -1e-3f)
                referenceSkew = *g_stereoBaselineAddress / scale;
        }
        const float magnitude =
            referenceSkew < 0.0f ? -referenceSkew : referenceSkew;
        const auto present = g_presentCount.load(std::memory_order_relaxed);
        // Ceiling 0.6: the BASELINE_10X preset is 0.52121, which a 0.5
        // ceiling silently rejected.
        if (!(referenceSkew == referenceSkew) || magnitude < 1e-4f ||
            magnitude > 0.6f)
            return;
        // dP[2][0] = -skew/2 lands as A[i][0] += (skew/2) * A[i][3] because
        // the composing matrix's column 2 equals minus the block's column 3.
        const float delta = 0.5f * referenceSkew;
        constexpr float kDepthScale = 1.000017f;
        constexpr float kDepthNear = -0.200003f;
        int matchedBlocks = 0;
        // Small CBs hold matrices at 64-byte offsets; upload arenas
        // sub-allocate at 16-byte alignment, so large buffers scan at the
        // finer stride to avoid missing the camera constants.
        const std::uint32_t stride = byteWidth > 4096 ? 16u : 64u;
        for (std::uint32_t offset = 0; offset + 64 <= byteWidth; offset += stride)
        {
            float* block = dataFloats + offset / 4;
            // Fail closed on non-finite data: NaN passes every ordered
            // comparison written as "error > tol", and this engine does emit
            // NaN constant rows.  A poisoned block must never be patched or
            // treated as having an inverse neighbor.
            bool finite = true;
            for (int index = 0; index < 16 && finite; ++index)
            {
                const float value = block[index];
                if (!(value == value) || !(value - value == 0.0f))
                    finite = false;
            }
            if (!finite)
                continue;
            bool match = true;
            for (int row = 0; row < 3 && match; ++row)
            {
                const float lhs = block[row * 4 + 2];
                const float expected = kDepthScale * block[row * 4 + 3];
                float error = lhs - expected;
                if (error < 0.0f)
                    error = -error;
                float scale = lhs < 0.0f ? -lhs : lhs;
                if (scale < 1.0f)
                    scale = 1.0f;
                if (error > 1e-3f * scale)
                    match = false;
            }
            if (match)
            {
                // Row 3 checks the recovered near constant absolutely: the
                // translation terms cancel algebraically, so true matches sit
                // at float noise while a different near plane fails at any
                // camera distance (the relative form degraded ~1600 units
                // from the origin).
                float recoveredNear =
                    block[14] - kDepthScale * block[15] - kDepthNear;
                if (recoveredNear < 0.0f)
                    recoveredNear = -recoveredNear;
                if (recoveredNear > 1e-2f)
                    match = false;
            }
            if (!match)
                continue;
            // View-context layouts (992-byte and scene-dependent variants)
            // store inv(A) in the next 64-byte block.  Detect by product
            // before patching so the pair stays coherent afterwards.  The
            // tolerance is relative to the accumulated magnitude because
            // float32 quantization of the stored pair grows with distance
            // from the world origin.
            bool hasAdjacentInverse = false;
            float* nextBlock = block + 16;
            if (offset + 128 <= byteWidth)
            {
                bool nextFinite = true;
                for (int index = 0; index < 16 && nextFinite; ++index)
                {
                    const float value = nextBlock[index];
                    if (!(value == value) || !(value - value == 0.0f))
                        nextFinite = false;
                }
                if (nextFinite)
                {
                    bool withinTolerance = true;
                    for (int row = 0; row < 4 && withinTolerance; ++row)
                        for (int column = 0; column < 4 && withinTolerance;
                            ++column)
                        {
                            float cell = 0.0f;
                            float cellScale = 0.0f;
                            for (int k = 0; k < 4; ++k)
                            {
                                const float term = block[row * 4 + k] *
                                    nextBlock[k * 4 + column];
                                cell += term;
                                cellScale += term < 0.0f ? -term : term;
                            }
                            const float target = row == column ? 1.0f : 0.0f;
                            float cellError = cell - target;
                            if (cellError < 0.0f)
                                cellError = -cellError;
                            const float tolerance = 5e-2f *
                                (cellScale > 1.0f ? cellScale : 1.0f);
                            if (cellError > tolerance)
                                withinTolerance = false;
                        }
                    hasAdjacentInverse = withinTolerance;
                }
            }
            for (int row = 0; row < 4; ++row)
                block[row * 4] += delta * block[row * 4 + 3];
            if (hasAdjacentInverse)
            {
                alignas(16) float inverse[16]{};
                if (InvertMatrix4x4(block, inverse))
                {
                    for (int index = 0; index < 16; ++index)
                        nextBlock[index] = inverse[index];
                }
                else
                    g_o0SkewInverseFailed.fetch_add(1,
                        std::memory_order_relaxed);
            }
            ++matchedBlocks;
        }
        if (!matchedBlocks)
            return;
        g_objSkewBlocks.fetch_add(static_cast<std::uint64_t>(matchedBlocks),
            std::memory_order_relaxed);
        g_objSkewWrites.fetch_add(1, std::memory_order_relaxed);
        for (auto& slot : g_objSkewResources)
        {
            std::uintptr_t expected = 0;
            const auto value = reinterpret_cast<std::uintptr_t>(resource);
            if (slot.compare_exchange_strong(expected, value,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                Log("[OBJSKEW] recenter resource=%p bytes=%u blocks=%d "
                    "delta=%.6f present=%llu",
                    resource, byteWidth, matchedBlocks, delta,
                    static_cast<unsigned long long>(present));
                break;
            }
            if (expected == value)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// [IVFOV] classify a 64-byte block as a perspective projection.  Returns 0
// for no match, 1 for row-vector storage (w column at indices 3/7/11/15,
// matching the camera-block convention where skew sits at P[2][0]=index 8),
// 2 for transposed storage (w row at indices 12..15).  Orthographic and UI
// blocks fail the unit-w test and are never touched.
int IvFovClassifyProjection(const float* block)
{
    for (int index = 0; index < 16; ++index)
    {
        const float value = block[index];
        if (!(value == value) || !(value - value == 0.0f))
            return 0;
    }
    const float p00 = block[0];
    const float p11 = block[5];
    if (!(p00 > 0.05f && p00 < 8.0f && p11 > 0.05f && p11 < 8.0f))
        return 0;
    constexpr float kZeroEps = 1e-4f;
    constexpr float kOneEps = 1e-3f;
    if (std::fabs(block[1]) < kZeroEps && std::fabs(block[2]) < kZeroEps &&
        std::fabs(block[3]) < kZeroEps && std::fabs(block[4]) < kZeroEps &&
        std::fabs(block[6]) < kZeroEps && std::fabs(block[7]) < kZeroEps &&
        std::fabs(block[12]) < kZeroEps && std::fabs(block[13]) < kZeroEps &&
        std::fabs(block[15]) < kZeroEps &&
        std::fabs(std::fabs(block[11]) - 1.0f) < kOneEps)
        return 1;
    if (std::fabs(block[1]) < kZeroEps && std::fabs(block[3]) < kZeroEps &&
        std::fabs(block[4]) < kZeroEps && std::fabs(block[7]) < kZeroEps &&
        std::fabs(block[8]) < kZeroEps && std::fabs(block[9]) < kZeroEps &&
        std::fabs(block[12]) < kZeroEps && std::fabs(block[13]) < kZeroEps &&
        std::fabs(block[15]) < kZeroEps &&
        std::fabs(std::fabs(block[14]) - 1.0f) < kOneEps)
        return 2;
    return 0;
}

// [IVFOV] find or claim the census cell for one CB size.
IvFovSizeCensusCell* IvFovCellFor(std::uint32_t byteWidth)
{
    for (auto& cell : g_ivFovSizeCensus)
    {
        std::uint32_t expected = 0;
        if (cell.size.compare_exchange_strong(expected, byteWidth,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return &cell;
        if (expected == byteWidth)
            return &cell;
    }
    return nullptr;
}

// [IVFOV] census: while the override is enabled, note every CB size whose
// uploads carry a perspective projection so a failed visual test still names
// the next target from the same run.  Buffers above 4096 bytes are upload
// arenas that sub-allocate at 16-byte alignment, so they scan at the finer
// stride.  The MAINCAM watch separately flags any projection whose vertical
// focal sits in the main-camera family (p11 1.2-2.6 at ~16:9), because the
// dominant patched frustums (p11~0.95) are a wide auxiliary class, not the
// world camera.
void IvFovCensusScan(const float* dataFloats, std::uint32_t byteWidth,
    void* resource, PoseBufferSample* sample, const char* lane)
{
    __try
    {
        const std::uint32_t stride = byteWidth > 4096 ? 16u : 64u;
        bool cellRecorded = false;
        for (std::uint32_t offset = 0; offset + 64 <= byteWidth;
            offset += stride)
        {
            const float* block = dataFloats + offset / 4;
            const int kind = IvFovClassifyProjection(block);
            if (!kind)
                continue;
            if (!cellRecorded)
            {
                cellRecorded = true;
                if (sample)
                    sample->ivProjSeen.fetch_or(1, std::memory_order_relaxed);
                auto* cell = IvFovCellFor(byteWidth);
                if (cell &&
                    cell->count.fetch_add(1, std::memory_order_relaxed) == 0)
                    Log("[IVFOV] census size=%u off=+0x%X kind=%d p00=%.4f "
                        "p11=%.4f res=%p bind=0x%X lane=%s",
                        byteWidth, offset, kind, block[0], block[5], resource,
                        sample ? sample->bindFlags : 0u, lane);
            }
            const float p00 = block[0];
            const float p11 = block[5];
            const float aspect = p00 > 0.001f ? p11 / p00 : 0.0f;
            if (p11 > 1.2f && p11 < 2.6f && aspect > 1.5f && aspect < 2.1f)
            {
                // One MAINCAM line per resource so a single camera
                // transition cannot exhaust the whole log budget.
                const bool firstForResource = sample &&
                    (sample->ivProjSeen.fetch_or(2,
                        std::memory_order_relaxed) & 2) == 0;
                if ((firstForResource || !sample) &&
                    g_ivFovMainCamLogs.fetch_add(1,
                        std::memory_order_relaxed) < 40)
                    Log("[IVFOV] MAINCAM size=%u off=+0x%X kind=%d p00=%.4f "
                        "p11=%.4f res=%p bind=0x%X lane=%s",
                        byteWidth, offset, kind, p00, p11, resource,
                        sample ? sample->bindFlags : 0u, lane);
                return;
            }
            if (cellRecorded && byteWidth <= 4096)
                return;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// [IVFOV] state dump: census totals plus the CB correlation table - which
// buffers bind for vertex work and whether a projection was ever seen or
// patched inside them.  Runs periodically while the override is on so an
// abrupt exit still leaves evidence in the log.
void IvFovDumpState(const char* reason)
{
    Log("[IVFOV] dump reason=%s patched=%llu vp=%llu inv=%llu bigSkip=%llu "
        "nonDiscard=%llu copy1=%llu copies=%llu fovApplies=%llu",
        reason,
        static_cast<unsigned long long>(
            g_ivFovPatched.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_ivFovVpPatched.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_ivFovInvPatched.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_ivFovSkippedPartial.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_ivFovNonDiscardMaps.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_ivFovCopy1Calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_ivFovCopyCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_fovScaleApplies.load(std::memory_order_relaxed)));
    for (auto& cell : g_ivFovSizeCensus)
    {
        const auto size = cell.size.load(std::memory_order_acquire);
        if (size)
            Log("[IVFOV] totals size=%u seen=%llu patched=%llu",
                size,
                static_cast<unsigned long long>(
                    cell.count.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    cell.patched.load(std::memory_order_relaxed)));
    }
    std::uint32_t lines = 0;
    for (auto& sample : g_poseBuffers)
    {
        const auto key = sample.resource.load(std::memory_order_acquire);
        if (!key)
            continue;
        const auto projSeen =
            sample.ivProjSeen.load(std::memory_order_relaxed);
        const auto patchedFlag =
            sample.ivPatchedFlag.load(std::memory_order_relaxed);
        const auto binds = sample.ivBinds.load(std::memory_order_relaxed);
        const auto bindOffset =
            sample.ivBindOffset.load(std::memory_order_relaxed);
        const bool big = sample.byteWidth > 4096;
        if (!projSeen && !patchedFlag && !(binds && big) && !bindOffset)
            continue;
        Log("[IVFOV] cb res=%p bytes=%u bind=0x%X binds=%llu lastSlot=%u "
            "bindOff=%u proj=%u patched=%u",
            reinterpret_cast<void*>(key), sample.byteWidth, sample.bindFlags,
            static_cast<unsigned long long>(binds),
            sample.ivLastSlot.load(std::memory_order_relaxed), bindOffset,
            projSeen, patchedFlag);
        if (++lines >= 40)
        {
            Log("[IVFOV] cb table truncated at 40 rows");
            break;
        }
    }
}

float CamParamAsFloat(std::uint32_t raw);

// [LENS] Report the vertical/horizontal FOV of any main-camera-band
// projection found in a constant-buffer upload.  Read-only.
void LensScanPayload(const float* dataFloats, std::uint32_t byteWidth,
    const char* lane)
{
    __try
    {
        const std::uint32_t stride = byteWidth > 4096 ? 16u : 64u;
        for (std::uint32_t offset = 0; offset + 64 <= byteWidth;
            offset += stride)
        {
            const float* block = dataFloats + offset / 4;
            if (!IvFovClassifyProjection(block))
                continue;
            const float p00 = block[0];
            const float p11 = block[5];
            if (!(p11 >= kLensP11Low) || !(p11 <= kLensP11High))
                continue;
            constexpr float kPi = 3.14159265358979323846f;
            const float fovV = 2.0f * std::atan(1.0f / p11) * 180.0f / kPi;
            const float fovH = 2.0f * std::atan(1.0f / p00) * 180.0f / kPi;
            g_lensSamples.fetch_add(1, std::memory_order_relaxed);
            if (byteWidth == 512 &&
                g_lensFeedEnabled.load(std::memory_order_relaxed) &&
                g_aerEnabled.load(std::memory_order_relaxed))
            {
                // Accumulate only; publication happens once per present in
                // Tick(), so the claim is exactly this frame's widest
                // main-band lens and can never latch a stale one.
                const auto lensPresent =
                    g_presentCount.load(std::memory_order_relaxed);
                if (g_lensFramePresent.exchange(lensPresent,
                        std::memory_order_acq_rel) != lensPresent)
                    ResetLensBuckets();
                AddLensSample(p11, p00, offset);
            }
            if constexpr (!ResearchDiagnostics::Enabled) return;
            const auto present = g_presentCount.load(std::memory_order_relaxed);
            const auto lastPresent =
                g_lensLastLogPresent.load(std::memory_order_relaxed);
            const float previous =
                g_lensLastVertical.load(std::memory_order_relaxed);
            const bool moved = std::fabs(fovV - previous) > 1.0f;
            if (!moved && present - lastPresent < 180)
                return;
            auto expected = lastPresent;
            if (!g_lensLastLogPresent.compare_exchange_strong(expected, present,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
                return;
            g_lensLastVertical.store(fovV, std::memory_order_relaxed);
            if (g_lensLogs.fetch_add(1, std::memory_order_relaxed) < 2000)
                Log("[LENS] fovV=%.2f fovH=%.2f p00=%.4f p11=%.4f size=%u "
                    "lane=%s slot7=%d present=%llu",
                    fovV, fovH, p00, p11, byteWidth, lane,
                    static_cast<int>(CamParamAsFloat(
                        g_camParamLastWritten.load(std::memory_order_relaxed))),
                    static_cast<unsigned long long>(present));
            return;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// [IVFOV] scale every perspective projection inside one CB payload, keeping
// companion matrices coherent: a second projection block scales the same
// way, a reciprocal inverse-projection block scales inversely, and any
// ViewProj-composed block (unit rotation w column) scales on its focal
// columns.  Rigid view/world blocks have a zero w column and are untouched
// by construction.  Pure relative scaling; no size gate, no baseline.
bool IvFovScalePayload(float* dataFloats, std::uint32_t byteWidth,
    void* resource, const char* lane)
{
    const float target = g_liveFovDegrees.load(std::memory_order_acquire);
    if (!(target > 10.0f) || !(target < 179.0f))
        return false;
    constexpr float kPi = 3.14159265358979323846f;
    const float scale = std::tan(kIvFovReferenceDegrees * 0.5f * kPi / 180.0f) /
        std::tan(target * 0.5f * kPi / 180.0f);
    if (scale > 0.995f && scale < 1.005f)
        return false;
    bool patched = false;
    __try
    {
        std::uint32_t projOffset = 0;
        int projKind = 0;
        float originalP00 = 0.0f;
        float originalP11 = 0.0f;
        for (std::uint32_t offset = 0; offset + 64 <= byteWidth; offset += 64)
        {
            const int kind = IvFovClassifyProjection(dataFloats + offset / 4);
            if (kind)
            {
                projOffset = offset;
                projKind = kind;
                originalP00 = dataFloats[offset / 4];
                originalP11 = dataFloats[offset / 4 + 5];
                break;
            }
        }
        if (!projKind)
            return false;
        auto* cell = IvFovCellFor(byteWidth);
        const bool dumpLayout = cell &&
            !cell->dumped.exchange(true, std::memory_order_acq_rel);
        std::uint32_t projBlocks = 0;
        std::uint32_t vpBlocks = 0;
        std::uint32_t invBlocks = 0;
        for (std::uint32_t offset = 0; offset + 64 <= byteWidth; offset += 64)
        {
            float* block = dataFloats + offset / 4;
            const char* action = "none";
            bool finite = true;
            for (int index = 0; index < 16 && finite; ++index)
            {
                const float value = block[index];
                if (!(value == value) || !(value - value == 0.0f))
                    finite = false;
            }
            if (finite)
            {
                const int kind = IvFovClassifyProjection(block);
                const float d0 = block[0] * originalP00 - 1.0f;
                const float d1 = block[5] * originalP11 - 1.0f;
                const float a = projKind == 1 ? block[3] : block[12];
                const float b = projKind == 1 ? block[7] : block[13];
                const float c = projKind == 1 ? block[11] : block[14];
                const float wNorm = a * a + b * b + c * c;
                if (kind)
                {
                    block[0] *= scale;
                    block[5] *= scale;
                    if (kind == 1)
                    {
                        block[8] *= scale;
                        block[9] *= scale;
                    }
                    else
                    {
                        block[2] *= scale;
                        block[6] *= scale;
                    }
                    ++projBlocks;
                    action = "proj";
                }
                else if (std::fabs(d0) < 0.01f && std::fabs(d1) < 0.01f)
                {
                    block[0] /= scale;
                    block[5] /= scale;
                    ++invBlocks;
                    action = "inv";
                }
                else if (wNorm > 0.81f && wNorm < 1.21f)
                {
                    if (projKind == 1)
                    {
                        for (int row = 0; row < 4; ++row)
                        {
                            block[row * 4] *= scale;
                            block[row * 4 + 1] *= scale;
                        }
                    }
                    else
                    {
                        for (int column = 0; column < 8; ++column)
                            block[column] *= scale;
                    }
                    ++vpBlocks;
                    action = "vp";
                }
            }
            if (dumpLayout)
                Log("[IVFOV] layout size=%u off=+0x%X act=%s m0=%.4f m3=%.4f "
                    "m5=%.4f m11=%.4f m12=%.4f m14=%.4f m15=%.4f",
                    byteWidth, offset, action, block[0], block[3], block[5],
                    block[11], block[12], block[14], block[15]);
        }
        patched = true;
        g_ivFovVpPatched.fetch_add(vpBlocks, std::memory_order_relaxed);
        g_ivFovInvPatched.fetch_add(invBlocks, std::memory_order_relaxed);
        if (cell)
            cell->patched.fetch_add(1, std::memory_order_relaxed);
        const auto count =
            g_ivFovPatched.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 4 || (count % 900) == 0)
            Log("[IVFOV] patched size=%u res=%p lane=%s off=+0x%X kind=%d "
                "p00 %.4f->%.4f p11 %.4f->%.4f proj=%u vp=%u inv=%u "
                "target=%.1f hit=%llu",
                byteWidth, resource, lane, projOffset, projKind, originalP00,
                dataFloats[projOffset / 4], originalP11,
                dataFloats[projOffset / 4 + 5], projBlocks, vpBlocks,
                invBlocks, target, static_cast<unsigned long long>(count));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return patched;
}

void RecordBufferCopy(ID3D11Resource* destination, ID3D11Resource* source,
    std::uintptr_t caller, const char* kind);

std::atomic_uint32_t g_frameVsSetCb{};

// [CTXORIGIN] VSSetConstantBuffers fires ~4700 times a frame; the draw hooks
// beside it fire ~58.  That state traffic belongs to draws not otherwise seen, and
// it arrives holding the very thing needed: the CONTEXT those draws are
// issued on.  Record each distinct context vtable seen here once, and read
// back what its Draw and DrawIndexed slots actually point at.  If a busy
// context's vtable is one never patched, that explains the missing draws.
void STDMETHODCALLTYPE HookReplayDraw(ID3D11DeviceContext* context, UINT vertices,
    UINT start);
void STDMETHODCALLTYPE HookDrawIndexedCensus(ID3D11DeviceContext* context,
    UINT indexCount, UINT startIndex, INT baseVertex);
void STDMETHODCALLTYPE HookDrawIndexedInstancedCensus(ID3D11DeviceContext* context,
    UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex,
    UINT startInstance);
void STDMETHODCALLTYPE HookDrawInstancedCensus(ID3D11DeviceContext* context,
    UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex,
    UINT startInstance);
// HookReplayDraw resolves its original through the vtable registry, which
// cannot work for a per-object vtable it has never seen.  Keep the d3d11
// implementation in a global so a re-asserted slot can never drop a draw.
extern DrawIndexedFn g_realEngineDrawIndexed;
extern DrawIndexedInstancedFn g_realEngineDrawIndexedInstanced;
extern DrawInstancedFn g_realEngineDrawInstanced;
std::atomic_uint64_t g_drawSlotReasserts{};
constexpr std::size_t kCtxOriginSlots = 8;
std::atomic_uintptr_t g_ctxOriginVtables[kCtxOriginSlots]{};
std::atomic_uint32_t g_ctxOriginLogs{};

// [DRAWSLOT] Two facts, both read back from the log:
//
//   [CTXORIGIN] busy context 0ECE7A78 vtable 0ECE7A80 ... selfVtable=1
//   [TABLECHECK] table 1: VSSetCB=ours DrawIndexed=REVERTED ... (re-patched 3)
//
// The vtable pointer is eight bytes past the object, so it is a PER-OBJECT
// heap vtable, and the engine rewrites its three draw entries every few
// seconds while leaving VSSetConstantBuffers alone.  A slot patch on a timer
// cannot hold (the census then sees fifty-eight draws a frame beside twelve
// thousand state calls).  So re-assert the slots at a point that runs
// immediately before the draws themselves - VSSetConstantBuffers, which fires
// thousands of times per frame on that very context.
//
// Writing vtable slots raw from the hottest path in the frame is unsafe: a
// read-only vtable in a module's image raises an access violation on every
// call, and not every context's vtable is one of the heap ones this is for.
// Draw (13) is not needed - the policy function already sees every Draw.
//
// So this decides ONCE per distinct vtable address whether it may touch
// it at all: private (heap) memory only, never a module image, and writable.
// A rejected vtable is remembered and never written to again, so there is no
// exception storm.  Only the three INDEXED slots are asserted - those are the
// ones the census has never seen.  Draw is left alone.
constexpr std::size_t kDrawSlotVtables = 64;
std::atomic_uintptr_t g_drawSlotVtable[kDrawSlotVtables]{};
std::atomic_uint32_t g_drawSlotVerdict[kDrawSlotVtables]{};   // 1 = ours to patch, 2 = hands off
std::atomic_uint32_t g_drawSlotLogs{};

// One-time judgement on a vtable.  Heap-allocated and writable, or nothing.
MEMORY_BASIC_INFORMATION g_lastJudged{};

std::uint32_t JudgeVtable(std::uintptr_t vtable)
{
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(vtable), &info, sizeof(info)))
        return 2;
    g_lastJudged = info;
    if (info.State != MEM_COMMIT)
        return 2;
    if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return 2;
    // Module-image vtables are accepted: the drawing context uses the shared
    // d3d11 vtable, which lives in d3d11.dll's image and is read-only.
    // scene_census's Patch calls VirtualProtect for those.  What matters is
    // calling the per-vtable original, never a single global one.
    return 1;
}

// Remember that a vtable has already been handed to the census, so the offer
// happens once per vtable rather than once per call.
void MarkDrawSlotOffered(std::uintptr_t vtable)
{
    for (std::size_t i = 0; i < kDrawSlotVtables; ++i)
        if (g_drawSlotVtable[i].load(std::memory_order_acquire) == vtable)
        {
            g_drawSlotVerdict[i].store(3, std::memory_order_release);
            return;
        }
}

std::uint32_t DrawSlotVerdict(std::uintptr_t vtable)
{
    for (std::size_t i = 0; i < kDrawSlotVtables; ++i)
    {
        auto existing = g_drawSlotVtable[i].load(std::memory_order_acquire);
        if (existing == vtable)
            return g_drawSlotVerdict[i].load(std::memory_order_acquire);
        if (existing == 0)
        {
            if (!g_drawSlotVtable[i].compare_exchange_strong(existing, vtable,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                if (existing != vtable)
                    continue;
                return g_drawSlotVerdict[i].load(std::memory_order_acquire);
            }
            const auto verdict = JudgeVtable(vtable);
            const auto info = g_lastJudged;
            g_drawSlotVerdict[i].store(verdict, std::memory_order_release);
            if (g_drawSlotLogs.fetch_add(1, std::memory_order_relaxed) < 12)
                Log("[DRAWSLOT] vtable %p state=%lX type=%lX protect=%lX -> %s",
                    reinterpret_cast<void*>(vtable),
                    static_cast<unsigned long>(info.State),
                    static_cast<unsigned long>(info.Type),
                    static_cast<unsigned long>(info.Protect),
                    verdict == 1 ? "will offer to the census" : "refused");
            return verdict;
        }
    }
    return 2;   // more vtables than expected; do not guess
}

void EnsureDrawHooksOnContext(ID3D11DeviceContext* context)
{
    std::uintptr_t vtable = 0;
    __try
    {
        vtable = *reinterpret_cast<std::uintptr_t*>(context);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    if (!vtable || DrawSlotVerdict(vtable) != 1)
        return;
    auto* table = reinterpret_cast<void**>(vtable);
    // Only the indexed entry points.  Draw already arrives through the policy
    // function, and PatchVtable will not store one of the mod's own hooks as the
    // original, so a re-entry can never build a loop through here.
    const bool a = PatchVtable(table, 12,
        reinterpret_cast<void*>(&HookDrawIndexedCensus),
        reinterpret_cast<void**>(&g_realEngineDrawIndexed));
    const bool b = PatchVtable(table, 20,
        reinterpret_cast<void*>(&HookDrawIndexedInstancedCensus),
        reinterpret_cast<void**>(&g_realEngineDrawIndexedInstanced));
    const bool c = PatchVtable(table, 21,
        reinterpret_cast<void*>(&HookDrawInstancedCensus),
        reinterpret_cast<void**>(&g_realEngineDrawInstanced));
    if (a || b || c)
        g_drawSlotReasserts.fetch_add(1, std::memory_order_relaxed);
}

// Resolve a function pointer to the module that owns it, plus its offset.
// A bare address proves nothing; "d3d11.dll+0x1234" versus "dxgi.dll+0x1234"
// is the whole difference between a clean chain and cutting this mod's own hook out
// of one.
void LogPointerOwner(const char* tag, int slot, std::uintptr_t address)
{
    if (!address)
    {
        Log("%s %d = null", tag, slot);
        return;
    }
    HMODULE module{};
    char name[MAX_PATH]{};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module) && module &&
        GetModuleFileNameA(module, name, MAX_PATH))
    {
        const char* leaf = std::strrchr(name, '\\');
        leaf = leaf ? leaf + 1 : name;
        Log("%s %d = %s+0x%llX  (%p)", tag, slot, leaf,
            static_cast<unsigned long long>(address -
                reinterpret_cast<std::uintptr_t>(module)),
            reinterpret_cast<void*>(address));
    }
    else
        Log("%s %d = <no module> %p", tag, slot,
            reinterpret_cast<void*>(address));
}

void NoteContextOrigin(ID3D11DeviceContext* context)
{
    if (!context)
        return;
    std::uintptr_t vtable = 0;
    __try
    {
        vtable = *reinterpret_cast<std::uintptr_t*>(context);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    if (!vtable)
        return;
    for (auto& slot : g_ctxOriginVtables)
    {
        auto existing = slot.load(std::memory_order_acquire);
        if (existing == vtable)
            return;
        if (existing == 0 && slot.compare_exchange_strong(existing, vtable,
                std::memory_order_acq_rel, std::memory_order_acquire))
            break;
        if (slot.load(std::memory_order_acquire) == vtable)
            return;
    }
    if (g_ctxOriginLogs.fetch_add(1, std::memory_order_relaxed) >= 16)
        return;
    __try
    {
        auto* table = reinterpret_cast<std::uintptr_t*>(vtable);
        Log("[CTXORIGIN] busy context %p vtable %p selfVtable=%d",
            reinterpret_cast<void*>(context), reinterpret_cast<void*>(vtable),
            vtable > reinterpret_cast<std::uintptr_t>(context) &&
                vtable < reinterpret_cast<std::uintptr_t>(context) + 0x1000 ? 1 : 0);
        // Replacing these slots and calling d3d11's implementation instead
        // crashes the game on load unless the slot really held a raw d3d11
        // pointer.  Name the owner of every pointer.
        const std::size_t slots[] = {12, 13, 20, 21, 38, 39, 40, 41};
        for (const auto slot : slots)
            LogPointerOwner("[CTXORIGIN] slot", static_cast<int>(slot), table[slot]);
        LogPointerOwner("[CTXORIGIN] ours ", 12,
            reinterpret_cast<std::uintptr_t>(&HookDrawIndexedCensus));
        LogPointerOwner("[CTXORIGIN] ours ", 13,
            reinterpret_cast<std::uintptr_t>(&HookReplayDraw));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
std::atomic_uint32_t g_frameMaps{};
std::atomic_uint32_t g_frameUpdates{};
std::atomic_uint32_t g_frameDispatches{};

void STDMETHODCALLTYPE HookUpdateSubresource1(ID3D11DeviceContext* context,
    ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box,
    const void* source, UINT rowPitch, UINT depthPitch, UINT copyFlags)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    alignas(16) std::uint8_t patchCopy[4096];
    const void* forwardSource = source;
    auto* sample = FindPoseBuffer(resource, caller, true);
    if (kEnableIvFovPatchLane && source && sample &&
        g_liveFovOverrideEnabled.load(std::memory_order_acquire))
    {
        std::uint32_t payloadBytes = sample->byteWidth;
        if (box && box->right > box->left)
            payloadBytes = box->right - box->left;
        if (payloadBytes >= 64 && payloadBytes <= (8u << 20))
            IvFovCensusScan(reinterpret_cast<const float*>(source),
                payloadBytes, resource, sample, "update1");
        if ((sample->bindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 &&
            payloadBytes >= 64 && payloadBytes <= sizeof(patchCopy))
        {
            bool copied = false;
            __try
            {
                std::memcpy(patchCopy, source, payloadBytes);
                copied = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            if (copied && IvFovScalePayload(
                    reinterpret_cast<float*>(patchCopy), payloadBytes,
                    resource, "update1"))
            {
                sample->ivPatchedFlag.store(1, std::memory_order_relaxed);
                forwardSource = patchCopy;
            }
        }
        else if (payloadBytes > sizeof(patchCopy))
            g_ivFovSkippedPartial.fetch_add(1, std::memory_order_relaxed);
    }
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->updateSubresource1)
        hook->updateSubresource1(context, resource, subresource, box,
            forwardSource, rowPitch, depthPitch, copyFlags);
}

void STDMETHODCALLTYPE HookCopySubresourceRegion1(ID3D11DeviceContext* context,
    ID3D11Resource* destination, UINT destinationSubresource, UINT destX,
    UINT destY, UINT destZ, ID3D11Resource* source, UINT sourceSubresource,
    const D3D11_BOX* sourceBox, UINT copyFlags)
{
    g_ivFovCopy1Calls.fetch_add(1, std::memory_order_relaxed);
    RecordBufferCopy(destination, source,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()), "region1");
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->copySubresourceRegion1)
        hook->copySubresourceRegion1(context, destination,
            destinationSubresource, destX, destY, destZ, source,
            sourceSubresource, sourceBox, copyFlags);
}

void STDMETHODCALLTYPE HookVSSetConstantBuffers1(ID3D11DeviceContext* context,
    UINT startSlot, UINT bufferCount, ID3D11Buffer* const* buffers,
    const UINT* firstConstant, const UINT* numConstants)
{
    for (UINT index = 0; buffers && index < bufferCount; ++index)
    {
        auto* buffer = buffers[index];
        if (!buffer)
            continue;
        auto* sample = FindPoseBuffer(buffer, 0, true);
        if (!sample)
            continue;
        const auto slot = startSlot + index;
        sample->ivBinds.fetch_add(1, std::memory_order_relaxed);
        sample->ivLastSlot.store(slot, std::memory_order_relaxed);
        if (firstConstant)
        {
            sample->ivBindOffset.store(firstConstant[index] * 16,
                std::memory_order_relaxed);
            if (g_ivFovVscb1Logs.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("[IVFOV] vscb1 slot=%u res=%p bytes=%u firstConstant=%u "
                    "numConstants=%u",
                    slot, static_cast<void*>(buffer), sample->byteWidth,
                    firstConstant[index],
                    numConstants ? numConstants[index] : 0);
        }
    }
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->vsSetConstantBuffers1)
        hook->vsSetConstantBuffers1(context, startSlot, bufferCount, buffers,
            firstConstant, numConstants);
}

HRESULT STDMETHODCALLTYPE HookMap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource, D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped)
{
    if constexpr (ResearchDiagnostics::Enabled) g_frameMaps.fetch_add(1, std::memory_order_relaxed);
    g_mappedPoseSample = nullptr;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    const HRESULT result = hook && hook->map
        ? hook->map(context, resource, subresource, mapType, mapFlags, mapped) : E_FAIL;
    if (SUCCEEDED(result) && mapped && mapType != D3D11_MAP_READ)
    {
        auto* sample = FindPoseBuffer(resource, caller, true);
        g_mappedPoseSample = sample;
        if (sample)
        {
            sample->mappedData.store(reinterpret_cast<std::uintptr_t>(mapped->pData),
                std::memory_order_release);
            sample->lastMapType.store(static_cast<std::uint32_t>(mapType),
                std::memory_order_relaxed);
            if (caller >= g_exeBase && caller < g_exeBase + kRetailImageSize &&
                (caller < g_exeBase + 0x02DB0000 || caller >= g_exeBase + 0x02DB4000))
                sample->producerMapCallerRva.store(caller - g_exeBase,
                    std::memory_order_relaxed);
            bool mapStackExpected = false;
            if (sample->byteWidth == kViewCbPayloadBytes &&
                g_viewCbPayloadResource.load(std::memory_order_acquire) ==
                    reinterpret_cast<std::uintptr_t>(resource) &&
                g_viewCbPayloadMapStackLogged.compare_exchange_strong(
                    mapStackExpected, true, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                void* frames[8]{};
                const auto frameCount = CaptureStackBackTrace(1, 8, frames, nullptr);
                std::uintptr_t rvas[8]{};
                for (USHORT index = 0; index < frameCount; ++index)
                {
                    const auto address =
                        reinterpret_cast<std::uintptr_t>(frames[index]);
                    if (address >= g_exeBase && address < g_exeBase + kRetailImageSize)
                        rvas[index] = address - g_exeBase;
                }
                Log("[VSCB992_MAPSTACK] resource=%p thread=%u mapType=%u "
                    "frames=+%llX/+%llX/+%llX/+%llX/+%llX/+%llX/+%llX/+%llX",
                    resource, GetCurrentThreadId(),
                    static_cast<unsigned>(mapType),
                    static_cast<unsigned long long>(rvas[0]),
                    static_cast<unsigned long long>(rvas[1]),
                    static_cast<unsigned long long>(rvas[2]),
                    static_cast<unsigned long long>(rvas[3]),
                    static_cast<unsigned long long>(rvas[4]),
                    static_cast<unsigned long long>(rvas[5]),
                    static_cast<unsigned long long>(rvas[6]),
                    static_cast<unsigned long long>(rvas[7]));
            }
        }
    }
    return result;
}

void STDMETHODCALLTYPE HookUnmap(ID3D11DeviceContext* context, ID3D11Resource* resource,
    UINT subresource)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto* sample = FindPoseBuffer(resource, caller, false);
    if (sample)
    {
        if (caller >= g_exeBase && caller < g_exeBase + kRetailImageSize &&
            (caller < g_exeBase + 0x02DB0000 || caller >= g_exeBase + 0x02DB4000))
            sample->producerUnmapCallerRva.store(caller - g_exeBase,
                std::memory_order_relaxed);
        const auto data = sample->mappedData.exchange(0, std::memory_order_acq_rel);
        if (data)
            RecordPoseBuffer(sample, reinterpret_cast<const void*>(data), sample->byteWidth);
        if (data && sample->byteWidth == kViewCbPayloadBytes &&
            g_stereoEnabled.load(std::memory_order_relaxed) &&
            g_o0SkewProbeMode.load(std::memory_order_relaxed) ==
                kO0SkewModeSway)
            ProbeMainCameraPayload(reinterpret_cast<float*>(data), resource);
        if (data && sample->byteWidth >= 64 &&
            sample->byteWidth <= (8u << 20) &&
            g_stereoEnabled.load(std::memory_order_relaxed))
        {
            const auto probeMode =
                g_o0SkewProbeMode.load(std::memory_order_relaxed);
            // Patched blocks re-match the fingerprint, so only DISCARD maps
            // (guaranteed-fresh memory) may be patched in place; any other
            // map type could carry already-patched bytes and accumulate.
            const auto mapType =
                sample->lastMapType.load(std::memory_order_relaxed);
            if (mapType != static_cast<std::uint32_t>(D3D11_MAP_WRITE_DISCARD))
            {
                if (probeMode == kO0SkewModeProbe ||
                    probeMode == kO0SkewModeSwayObj)
                {
                    g_objSkewSkippedNonDiscard.fetch_add(1,
                        std::memory_order_relaxed);
                    bool logExpected = false;
                    if (g_objSkewNonDiscardLogged.compare_exchange_strong(
                            logExpected, true, std::memory_order_acq_rel,
                            std::memory_order_acquire))
                        Log("[OBJSKEW] non-discard map skipped resource=%p "
                            "bytes=%u mapType=%u",
                            resource, sample->byteWidth, mapType);
                }
            }
            else if (probeMode == kO0SkewModeSwayObj &&
                sample->byteWidth <= 4096 &&
                sample->byteWidth != kViewCbPayloadBytes)
                SwayProjectionBlocks(reinterpret_cast<float*>(data),
                    sample->byteWidth, resource,
                    sample->vsSlotsMask[1].load(std::memory_order_relaxed) |
                        sample->vsSlotsMask[2].load(std::memory_order_relaxed));
            else if (probeMode == kO0SkewModeProbe)
                UniformRecenterBlocks(reinterpret_cast<float*>(data),
                    sample->byteWidth, resource);
        }
        if (data && sample->byteWidth >= 64 && sample->byteWidth <= 4096 &&
            sample->lastMapType.load(std::memory_order_relaxed) ==
                static_cast<std::uint32_t>(D3D11_MAP_WRITE_DISCARD))
            LensScanPayload(reinterpret_cast<const float*>(data),
                sample->byteWidth, "map");
        if (kEnableIvFovPatchLane && data && sample->byteWidth >= 64 &&
            sample->byteWidth <= (8u << 20) &&
            g_liveFovOverrideEnabled.load(std::memory_order_acquire))
        {
            // Census covers EVERY mapped buffer, staging rings included -
            // bindFlags=0 upload arenas are the likely GPU-copy supply of the
            // always-bound 8192-byte CBs.  Patching stays restricted to discard-mapped
            // constant buffers.
            const auto ivMapType =
                sample->lastMapType.load(std::memory_order_relaxed);
            if (ivMapType == static_cast<std::uint32_t>(D3D11_MAP_WRITE_DISCARD))
            {
                IvFovCensusScan(reinterpret_cast<const float*>(data),
                    sample->byteWidth, resource, sample, "map");
                if ((sample->bindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 &&
                    sample->byteWidth <= 4096 &&
                    IvFovScalePayload(reinterpret_cast<float*>(data),
                        sample->byteWidth, resource, "map"))
                    sample->ivPatchedFlag.store(1, std::memory_order_relaxed);
            }
            else
            {
                // Non-discard maps may carry already-patched bytes, so they
                // are censused (read-only) but never patched in place.
                IvFovCensusScan(reinterpret_cast<const float*>(data),
                    sample->byteWidth, resource, sample, "map-nd");
                g_ivFovNonDiscardMaps.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (ResearchDiagnostics::Enabled && data && sample->byteWidth == kViewCbPayloadBytes &&
            g_viewCbPayloadResource.load(std::memory_order_acquire) ==
                reinterpret_cast<std::uintptr_t>(resource))
        {
            const auto trueOutput = static_cast<std::uint32_t>(ReadTrueOutput());
            const auto writeMode = g_stereoEnabled.load(std::memory_order_relaxed)
                ? static_cast<std::size_t>(1 + (trueOutput & 1)) : 0;
            std::uint64_t latestGeneration = 0;
            auto& snapshot = g_viewCbPayloadLatest;
            AcquireSRWLockExclusive(&snapshot.lock);
            __try
            {
                std::memcpy(snapshot.bytes.data(), reinterpret_cast<const void*>(data),
                    kViewCbPayloadBytes);
                snapshot.present = g_presentCount.load(std::memory_order_relaxed);
                snapshot.hash = sample->lastHash.load(std::memory_order_relaxed);
                snapshot.jobSequence =
                    g_sepProbeJobSequence.load(std::memory_order_relaxed);
                snapshot.threadId = GetCurrentThreadId();
                snapshot.trueOutput = trueOutput;
                latestGeneration =
                    snapshot.generation.fetch_add(1, std::memory_order_release) + 1;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            ReleaseSRWLockExclusive(&snapshot.lock);
            if (latestGeneration)
            {
                auto& destination = g_viewCbPayloadsAtWrite[writeMode];
                AcquireSRWLockExclusive(&destination.lock);
                __try
                {
                    std::memcpy(destination.bytes.data(),
                        reinterpret_cast<const void*>(data), kViewCbPayloadBytes);
                    destination.present = g_presentCount.load(std::memory_order_relaxed);
                    destination.hash = sample->lastHash.load(std::memory_order_relaxed);
                    destination.jobSequence =
                        g_sepProbeJobSequence.load(std::memory_order_relaxed);
                    destination.threadId = GetCurrentThreadId();
                    destination.trueOutput = trueOutput;
                    destination.sourceGeneration = latestGeneration;
                    destination.generation.fetch_add(1, std::memory_order_release);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
                ReleaseSRWLockExclusive(&destination.lock);
                const auto bit = 1u << writeMode;
                if ((g_viewCbPayloadStackModes.fetch_or(bit,
                        std::memory_order_acq_rel) & bit) == 0)
                {
                    void* frames[8]{};
                    const auto frameCount =
                        CaptureStackBackTrace(1, 8, frames, nullptr);
                    std::uintptr_t rvas[8]{};
                    for (USHORT index = 0; index < frameCount; ++index)
                    {
                        const auto address =
                            reinterpret_cast<std::uintptr_t>(frames[index]);
                        if (address >= g_exeBase &&
                            address < g_exeBase + kRetailImageSize)
                            rvas[index] = address - g_exeBase;
                    }
                    Log("[VSCB992_STACK] mode=%s resource=%p thread=%u "
                        "present=%llu generation=%llu "
                        "frames=+%llX/+%llX/+%llX/+%llX/+%llX/+%llX/+%llX/+%llX",
                        writeMode == 0 ? "mono" : writeMode == 1 ? "o0" : "o1",
                        resource, GetCurrentThreadId(),
                        static_cast<unsigned long long>(
                            g_presentCount.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(latestGeneration),
                        static_cast<unsigned long long>(rvas[0]),
                        static_cast<unsigned long long>(rvas[1]),
                        static_cast<unsigned long long>(rvas[2]),
                        static_cast<unsigned long long>(rvas[3]),
                        static_cast<unsigned long long>(rvas[4]),
                        static_cast<unsigned long long>(rvas[5]),
                        static_cast<unsigned long long>(rvas[6]),
                        static_cast<unsigned long long>(rvas[7]));
                }
            }
        }
        if (sample->byteWidth == 352)
        {
            const auto mode = g_stereoEnabled.load(std::memory_order_relaxed)
                ? static_cast<std::uint32_t>(1 + (ReadTrueOutput() & 1)) : 0u;
            const auto bit = 1u << mode;
            const auto previousModes =
                sample->stackLoggedModes.fetch_or(bit, std::memory_order_acq_rel);
            const bool firstForMode = (previousModes & bit) == 0;
            const auto logIndex = firstForMode
                ? g_viewCbStackLogs.fetch_add(1, std::memory_order_relaxed) : 24u;
            if (firstForMode && logIndex < 24)
            {
                void* frames[8]{};
                const auto frameCount = CaptureStackBackTrace(1, 8, frames, nullptr);
                std::uintptr_t rvas[8]{};
                for (USHORT index = 0; index < frameCount; ++index)
                {
                    const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
                    if (address >= g_exeBase && address < g_exeBase + kRetailImageSize)
                        rvas[index] = address - g_exeBase;
                }
                Log("[VIEWCB_STACK] mode=%s resource=%p caller=+0x%llX "
                    "frames=+%llX/+%llX/+%llX/+%llX/+%llX/+%llX/+%llX/+%llX",
                    mode == 0 ? "mono" : mode == 1 ? "o0" : "o1", resource,
                    static_cast<unsigned long long>(sample->producerUnmapCallerRva.load(
                        std::memory_order_relaxed)),
                    static_cast<unsigned long long>(rvas[0]),
                    static_cast<unsigned long long>(rvas[1]),
                    static_cast<unsigned long long>(rvas[2]),
                    static_cast<unsigned long long>(rvas[3]),
                    static_cast<unsigned long long>(rvas[4]),
                    static_cast<unsigned long long>(rvas[5]),
                    static_cast<unsigned long long>(rvas[6]),
                    static_cast<unsigned long long>(rvas[7]));
            }
        }
    }
    g_mappedPoseSample = nullptr;
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->unmap)
        hook->unmap(context, resource, subresource);
}

void STDMETHODCALLTYPE HookUpdateSubresource(ID3D11DeviceContext* context,
    ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box, const void* source,
    UINT rowPitch, UINT depthPitch)
{
    if constexpr (ResearchDiagnostics::Enabled) g_frameUpdates.fetch_add(1, std::memory_order_relaxed);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    alignas(16) std::uint8_t swayCopy[4096];
    const void* forwardSource = source;
    auto* sample = FindPoseBuffer(resource, caller, true);
    if (sample)
    {
        if (caller >= g_exeBase && caller < g_exeBase + kRetailImageSize)
        {
            sample->updateCallerRva.store(caller - g_exeBase,
                std::memory_order_relaxed);
            sample->updateSource.store(reinterpret_cast<std::uintptr_t>(source),
                std::memory_order_relaxed);
        }
        std::uint32_t size = sample->byteWidth;
        if (box && box->right > box->left)
            size = box->right - box->left;
        RecordPoseBuffer(sample, source, size);
        const auto probeMode = g_o0SkewProbeMode.load(std::memory_order_relaxed);
        const bool wantSway = probeMode == kO0SkewModeSwayObj &&
            sample->byteWidth != kViewCbPayloadBytes;
        const bool wantInject = probeMode == kO0SkewModeProbe;
        if (source && size >= 64 && size <= sizeof(swayCopy) &&
            g_stereoEnabled.load(std::memory_order_relaxed) &&
            (wantSway || wantInject))
        {
            bool copied = false;
            __try
            {
                std::memcpy(swayCopy, source, size);
                copied = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            if (copied)
            {
                if (wantSway)
                    // 0x80000000 marks the UpdateSubresource lane in [OBJSWAY].
                    SwayProjectionBlocks(reinterpret_cast<float*>(swayCopy),
                        size, resource, 0x80000000u |
                            sample->vsSlotsMask[1].load(
                                std::memory_order_relaxed) |
                            sample->vsSlotsMask[2].load(
                                std::memory_order_relaxed));
                else
                    UniformRecenterBlocks(reinterpret_cast<float*>(swayCopy),
                        size, resource);
                forwardSource = swayCopy;
            }
        }
    }
    if (source && sample &&
        (sample->bindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0)
    {
        // A boxed UpdateSubresource supplies only the box's bytes, not the
        // whole buffer.  Scanning byteWidth here overran the CALLER's memory
        // on every partial constant-buffer update - which loading screens do
        // constantly - and can crash the game during loads.
        std::uint32_t scanBytes = sample->byteWidth;
        if (box && box->right > box->left)
            scanBytes = box->right - box->left;
        if (scanBytes >= 64 && scanBytes <= 4096 &&
            scanBytes <= sample->byteWidth)
            LensScanPayload(reinterpret_cast<const float*>(source), scanBytes,
                "update");
    }
    if (kEnableIvFovPatchLane && source && sample && forwardSource == source &&
        g_liveFovOverrideEnabled.load(std::memory_order_acquire))
    {
        std::uint32_t payloadBytes = sample->byteWidth;
        if (box && box->right > box->left)
            payloadBytes = box->right - box->left;
        if (payloadBytes >= 64 && payloadBytes <= (8u << 20))
            IvFovCensusScan(reinterpret_cast<const float*>(source),
                payloadBytes, resource, sample, "update");
        if ((sample->bindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 &&
            payloadBytes >= 64 && payloadBytes <= sizeof(swayCopy))
        {
            bool ivCopied = false;
            __try
            {
                std::memcpy(swayCopy, source, payloadBytes);
                ivCopied = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            if (ivCopied && IvFovScalePayload(
                    reinterpret_cast<float*>(swayCopy), payloadBytes,
                    resource, "update"))
            {
                sample->ivPatchedFlag.store(1, std::memory_order_relaxed);
                forwardSource = swayCopy;
            }
        }
        else if (payloadBytes > sizeof(swayCopy))
            g_ivFovSkippedPartial.fetch_add(1, std::memory_order_relaxed);
    }
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->update)
        hook->update(context, resource, subresource, box, forwardSource, rowPitch,
            depthPitch);
}

void RecordBufferCopy(ID3D11Resource* destination, ID3D11Resource* source,
    std::uintptr_t caller, const char* kind)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!destination || !source)
        return;
    g_ivFovCopyCalls.fetch_add(1, std::memory_order_relaxed);
    auto* destSample = FindPoseBuffer(destination, 0, true);
    if (!destSample || destSample->byteWidth < 64 ||
        destSample->byteWidth > (1u << 20))
        return;
    auto* sourceSample = FindPoseBuffer(source, 0, true);
    // Correlation: a projection observed in the copy SOURCE marks the
    // destination as projection-fed even though the census cannot read the
    // destination's GPU-side bytes.
    if (sourceSample &&
        (sourceSample->ivProjSeen.load(std::memory_order_relaxed) & 1) != 0)
        destSample->ivProjSeen.fetch_or(4, std::memory_order_relaxed);
    // The always-bound 8192-byte CBs never see a CPU write, so copies into
    // large CBs get their own one-shot budget
    // instead of competing with the small-CB copies.
    auto& logBudget = destSample->byteWidth > 4096 ? g_ivFovCopyBigLogs
        : g_ivFovCopySmallLogs;
    if (logBudget.fetch_add(1, std::memory_order_acq_rel) >= 8)
        return;
    Log("[CBCOPY] %s dest=%p destBytes=%u destBind=0x%X src=%p srcBytes=%u "
        "srcBind=0x%X caller=+0x%llX",
        kind, static_cast<void*>(destination), destSample->byteWidth,
        destSample->bindFlags, static_cast<void*>(source),
        sourceSample ? sourceSample->byteWidth : 0u,
        sourceSample ? sourceSample->bindFlags : 0u,
        static_cast<unsigned long long>(
            caller >= g_exeBase && caller < g_exeBase + kRetailImageSize
                ? caller - g_exeBase : 0));
}

void STDMETHODCALLTYPE HookCopyResource(ID3D11DeviceContext* context,
    ID3D11Resource* destination, ID3D11Resource* source)
{
    RecordBufferCopy(destination, source,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()), "full");
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->copyResource)
        hook->copyResource(context, destination, source);
}

void STDMETHODCALLTYPE HookCopySubresourceRegion(ID3D11DeviceContext* context,
    ID3D11Resource* destination, UINT destSubresource, UINT destX, UINT destY,
    UINT destZ, ID3D11Resource* source, UINT sourceSubresource,
    const D3D11_BOX* box)
{
    RecordBufferCopy(destination, source,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()), "region");
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->copySubresourceRegion)
        hook->copySubresourceRegion(context, destination, destSubresource, destX,
            destY, destZ, source, sourceSubresource, box);
}

void STDMETHODCALLTYPE HookVSSetConstantBuffers(ID3D11DeviceContext* context,
    UINT startSlot, UINT bufferCount, ID3D11Buffer* const* buffers)
{
    if (!ResearchDiagnostics::Enabled &&
        g_o0SkewProbeMode.load(std::memory_order_relaxed) == kO0SkewModeSwayCam)
    {
        PerfCensus::Add(PerfCensus::OnPresentThread() ? PerfCensus::VsCbPt : PerfCensus::VsCbOther);
        if (g_realEngineVsSetConstantBuffers)
            g_realEngineVsSetConstantBuffers(context, startSlot, bufferCount, buffers);
        else
        {
            auto* hook = context
                ? FindContextVtableHook(*reinterpret_cast<void***>(context)) : nullptr;
            if (hook && hook->vsSetConstantBuffers)
                hook->vsSetConstantBuffers(context, startSlot, bufferCount, buffers);
        }
        return;
    }

    NoteContextOrigin(context);
    // [LATEHOOK] This hook fires on the contexts the game actually draws on -
    // twelve thousand times a frame - and those are the very contexts the
    // census never saw.  Offer each distinct vtable to scene_census's own
    // installer exactly once; it is idempotent and keeps per-vtable originals,
    // which is what makes it safe.
    const auto vtable = *reinterpret_cast<std::uintptr_t*>(context);
    if (DrawSlotVerdict(vtable) == 1)
    {
        SceneCensus::EnsureContextHooked(context);
        MarkDrawSlotOffered(vtable);
    }
    if constexpr (ResearchDiagnostics::Enabled) g_frameVsSetCb.fetch_add(1, std::memory_order_relaxed);
    const auto mode = g_stereoEnabled.load(std::memory_order_relaxed)
        ? static_cast<std::size_t>(1 + (ReadTrueOutput() & 1)) : 0;
    g_vsCbBindCalls[mode].fetch_add(1, std::memory_order_relaxed);

    ID3D11VertexShader* shader{};
    if (context)
        context->VSGetShader(&shader, nullptr, nullptr);
    const auto shaderIdentity = reinterpret_cast<std::uintptr_t>(shader);
    for (UINT index = 0; buffers && index < bufferCount; ++index)
    {
        auto* buffer = buffers[index];
        if (!buffer)
            continue;
        auto* sample = FindPoseBuffer(buffer, 0, true);
        if (!sample)
            continue;
        const auto slot = startSlot + index;
        sample->ivBinds.fetch_add(1, std::memory_order_relaxed);
        sample->ivLastSlot.store(slot, std::memory_order_relaxed);
        if (sample->byteWidth < 64 || sample->byteWidth > 4096)
            continue;
        if (sample->byteWidth == kViewCbPayloadBytes && slot == 2)
        {
            auto expected = static_cast<std::uintptr_t>(0);
            if (g_viewCbPayloadResource.compare_exchange_strong(expected,
                    reinterpret_cast<std::uintptr_t>(buffer),
                    std::memory_order_acq_rel, std::memory_order_acquire))
                Log("[VSCB992] selected resource=%p at VS slot 2", buffer);
        }
        const auto hash = sample->lastHash.load(std::memory_order_acquire);
        if (sample->byteWidth == kViewCbPayloadBytes && slot == 2 &&
            g_viewCbPayloadResource.load(std::memory_order_acquire) ==
                reinterpret_cast<std::uintptr_t>(buffer) &&
            g_viewCbPayloadLatest.generation.load(std::memory_order_acquire))
        {
            auto& destination = g_viewCbPayloads[mode];
            AcquireSRWLockShared(&g_viewCbPayloadLatest.lock);
            AcquireSRWLockExclusive(&destination.lock);
            destination.bytes = g_viewCbPayloadLatest.bytes;
            destination.present = g_presentCount.load(std::memory_order_relaxed);
            destination.hash = g_viewCbPayloadLatest.hash;
            destination.generation.fetch_add(1, std::memory_order_release);
            ReleaseSRWLockExclusive(&destination.lock);
            ReleaseSRWLockShared(&g_viewCbPayloadLatest.lock);
        }
        sample->vsBindCalls[mode].fetch_add(1, std::memory_order_relaxed);
        sample->vsSlotsMask[mode].fetch_or(
            1u << (slot & 31), std::memory_order_relaxed);
        sample->vsLastShader[mode].store(shaderIdentity, std::memory_order_relaxed);
        if (hash)
        {
            const auto previous = sample->vsLastHash[mode].exchange(
                hash, std::memory_order_acq_rel);
            if (previous && previous != hash)
                sample->vsHashChanges[mode].fetch_add(1, std::memory_order_relaxed);
            if (mode == 1 && sample->vsLastHash[2].load(
                    std::memory_order_acquire) == hash)
                sample->vsEqualO1AtO0.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (shader)
        shader->Release();

    if (g_realEngineVsSetConstantBuffers)
    {
        g_realEngineVsSetConstantBuffers(context, startSlot, bufferCount, buffers);
        return;
    }
    auto* hook = context
        ? FindContextVtableHook(*reinterpret_cast<void***>(context)) : nullptr;
    if (hook && hook->vsSetConstantBuffers)
        hook->vsSetConstantBuffers(context, startSlot, bufferCount, buffers);
}

void LogPoseBufferTelemetry(bool stereo)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    struct Candidate
    {
        PoseBufferSample* sample{};
        std::uint32_t reference{};
        std::uint32_t changes{};
        std::uint32_t writes{};
    };
    std::array<Candidate, 8> top{};
    std::uint32_t active{};
    std::uint32_t stale{};
    std::uint64_t writesTotal{};
    std::uint64_t changesTotal{};

    for (auto& sample : g_poseBuffers)
    {
        if (!sample.resource.load(std::memory_order_acquire))
            continue;
        const auto writes = sample.writes.exchange(0, std::memory_order_acq_rel);
        const auto changes = sample.changes.exchange(0, std::memory_order_acq_rel);
        writesTotal += writes;
        changesTotal += changes;
        if (writes)
            ++active;
        if (!stereo)
        {
            if (changes > sample.monoReference)
                sample.monoReference = changes;
            continue;
        }
        const auto reference = sample.monoReference;
        if (reference < 4 || static_cast<std::uint64_t>(changes) * 4 >= reference)
            continue;
        ++stale;
        Candidate candidate{&sample, reference, changes, writes};
        for (auto& ranked : top)
        {
            const auto score = static_cast<std::uint64_t>(candidate.reference) *
                (candidate.writes + 1) / (candidate.changes + 1);
            const auto rankedScore = ranked.sample
                ? static_cast<std::uint64_t>(ranked.reference) * (ranked.writes + 1) /
                    (ranked.changes + 1) : 0;
            if (!ranked.sample || score > rankedScore)
            {
                std::swap(candidate, ranked);
                if (!candidate.sample)
                    break;
            }
        }
    }

    Log("[POSEBUF] stereo=%d observed=%llu active=%u writes=%llu changes=%llu stale=%u",
        stereo ? 1 : 0,
        static_cast<unsigned long long>(g_poseBuffersObserved.load(std::memory_order_relaxed)),
        active, static_cast<unsigned long long>(writesTotal),
        static_cast<unsigned long long>(changesTotal), stale);
    for (auto& caller : g_poseRecorderCallers)
    {
        const auto rva = caller.rva.load(std::memory_order_acquire);
        const auto total = caller.total.exchange(0, std::memory_order_acq_rel);
        const auto size16 = caller.bytes16.exchange(0, std::memory_order_acq_rel);
        const auto size32 = caller.bytes32.exchange(0, std::memory_order_acq_rel);
        const auto size64 = caller.bytes64.exchange(0, std::memory_order_acq_rel);
        if (rva && (size16 || size32 || size64))
            Log("[POSERECORD] stereo=%d caller=+0x%llX total=%u sizes16/32/64=%u/%u/%u",
                stereo ? 1 : 0, static_cast<unsigned long long>(rva), total,
                size16, size32, size64);
    }
    if (!stereo)
        return;
    for (std::size_t rank = 0; rank < top.size() && top[rank].sample; ++rank)
    {
        const auto& candidate = top[rank];
        const auto& sample = *candidate.sample;
        Log("[POSEBUF] rank=%zu resource=%p source=%p writer=+0x%llX bytes=%u bind=0x%X "
            "monoChanges=%u stereo=%u/%u replay=+0x%llX record=+0x%llX "
            "producerMap=+0x%llX producerUnmap=+0x%llX update=+0x%llX updateSource=%p",
            rank + 1,
            reinterpret_cast<void*>(sample.resource.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(sample.replaySource.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.sourceWriterRva.load(
                std::memory_order_relaxed)),
            sample.byteWidth, sample.bindFlags, candidate.reference, candidate.changes,
            candidate.writes,
            static_cast<unsigned long long>(sample.callerRva.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.recordCallerRva.load(
                std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.producerMapCallerRva.load(
                std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.producerUnmapCallerRva.load(
                std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.updateCallerRva.load(
                std::memory_order_relaxed)),
            reinterpret_cast<void*>(sample.updateSource.load(std::memory_order_relaxed)));
    }
}

void LogVsConstantBufferCandidates(bool stereo)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!stereo)
        return;
    static std::atomic_uint64_t lastLogMs{};
    const auto now = GetTickCount64();
    auto previous = lastLogMs.load(std::memory_order_acquire);
    if (previous && now - previous < 5000)
        return;
    if (!lastLogMs.compare_exchange_strong(previous, now,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    struct Candidate
    {
        PoseBufferSample* sample{};
        std::uint64_t score{};
    };
    std::array<Candidate, 20> top{};
    std::uint32_t eligible{};
    for (auto& sample : g_poseBuffers)
    {
        if (!sample.resource.load(std::memory_order_acquire))
            continue;
        const auto o0Calls = sample.vsBindCalls[1].load(std::memory_order_relaxed);
        const auto o1Calls = sample.vsBindCalls[2].load(std::memory_order_relaxed);
        const auto o1Changes = sample.vsHashChanges[2].load(std::memory_order_relaxed);
        if (!o0Calls || !o1Calls || o1Changes < 2)
            continue;
        ++eligible;
        const auto equal = sample.vsEqualO1AtO0.load(std::memory_order_relaxed);
        Candidate candidate{&sample, (o1Changes + 1) * (equal + 1)};
        for (auto& ranked : top)
        {
            if (!ranked.sample || candidate.score > ranked.score)
            {
                std::swap(candidate, ranked);
                if (!candidate.sample)
                    break;
            }
        }
    }

    Log("[VSCB] binds mono/o0/o1=%llu/%llu/%llu eligible=%u",
        static_cast<unsigned long long>(g_vsCbBindCalls[0].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_vsCbBindCalls[1].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_vsCbBindCalls[2].load(std::memory_order_relaxed)),
        eligible);
    std::size_t rank{};
    for (const auto& candidate : top)
    {
        if (!candidate.sample)
            continue;
        const auto& sample = *candidate.sample;
        Log("[VSCB] rank=%zu resource=%p bytes=%u calls=%llu/%llu/%llu "
            "changes=%llu/%llu/%llu equalO1AtO0=%llu slots=0x%X/0x%X/0x%X "
            "hash=%016llX/%016llX shader=%p/%p writer=+0x%llX/+0x%llX",
            ++rank,
            reinterpret_cast<void*>(sample.resource.load(std::memory_order_relaxed)),
            sample.byteWidth,
            static_cast<unsigned long long>(sample.vsBindCalls[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.vsBindCalls[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.vsBindCalls[2].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.vsHashChanges[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.vsHashChanges[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.vsHashChanges[2].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.vsEqualO1AtO0.load(std::memory_order_relaxed)),
            sample.vsSlotsMask[0].load(std::memory_order_relaxed),
            sample.vsSlotsMask[1].load(std::memory_order_relaxed),
            sample.vsSlotsMask[2].load(std::memory_order_relaxed),
            static_cast<unsigned long long>(sample.vsLastHash[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.vsLastHash[2].load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(sample.vsLastShader[1].load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(sample.vsLastShader[2].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.producerMapCallerRva.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sample.producerUnmapCallerRva.load(std::memory_order_relaxed)));
    }
}
using PoseBufferRecorderFn = void(__fastcall*)(void*, void*, const void*, std::uint64_t,
    std::uint32_t);
PoseBufferRecorderFn g_realPoseBufferRecorder{};

void RecordPoseRecorderCaller(std::uintptr_t caller, std::uint64_t size)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (caller < g_exeBase || caller >= g_exeBase + kRetailImageSize)
        return;
    const auto rva = caller - g_exeBase;
    for (auto& slot : g_poseRecorderCallers)
    {
        auto existing = slot.rva.load(std::memory_order_acquire);
        if (existing == 0 && !slot.rva.compare_exchange_strong(existing, rva,
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        if (existing != 0 && existing != rva)
            continue;
        slot.total.fetch_add(1, std::memory_order_relaxed);
        if (size == 16)
            slot.bytes16.fetch_add(1, std::memory_order_relaxed);
        else if (size == 32)
            slot.bytes32.fetch_add(1, std::memory_order_relaxed);
        else if (size == 64)
            slot.bytes64.fetch_add(1, std::memory_order_relaxed);
        g_poseRecorderHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

extern "C" void __fastcall RetailPoseBufferRecorderPolicy(void* queue, void* wrapper,
    const void* source, std::uint64_t size, std::uint32_t flags)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    RecordPoseRecorderCaller(caller, size);
    __try
    {
        auto* resource = wrapper
            ? *reinterpret_cast<ID3D11Resource**>(static_cast<std::uint8_t*>(wrapper) + 0x30)
            : nullptr;
        if (auto* sample = FindPoseBuffer(resource, 0, true))
        {
            if (caller >= g_exeBase && caller < g_exeBase + kRetailImageSize)
                sample->recordCallerRva.store(caller - g_exeBase,
                    std::memory_order_relaxed);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (g_realPoseBufferRecorder)
        g_realPoseBufferRecorder(queue, wrapper, source, size, flags);
}

void STDMETHODCALLTYPE HookExecuteCommandList(ID3D11DeviceContext* context,
    ID3D11CommandList* commandList, BOOL restoreState)
{
    g_commandListsExecuted.fetch_add(1, std::memory_order_relaxed);
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->execute)
    {
        hook->execute(context, commandList, restoreState);
    }
}

void RecordDispatchCaller(std::uintptr_t rva, std::uintptr_t shader, UINT x, UINT y, UINT z)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!rva)
        return;
    std::uint64_t key = 1469598103934665603ull;
    const std::uint64_t values[] = {rva, shader, x, y, z};
    for (const auto value : values)
        key = (key ^ value) * 1099511628211ull;
    if (key == 0)
        key = 1;
    const auto start = static_cast<std::size_t>((key ^ (key >> 32)) &
        (kDispatchCallerSampleCount - 1));
    for (std::size_t probe = 0; probe < 32; ++probe)
    {
        auto& slot = g_dispatchCallers[(start + probe) & (kDispatchCallerSampleCount - 1)];
        auto existing = slot.key.load(std::memory_order_acquire);
        if (existing == 0)
        {
            if (!slot.key.compare_exchange_strong(existing, key,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            slot.rva = rva;
            slot.shader = shader;
            slot.x = x;
            slot.y = y;
            slot.z = z;
            slot.ready.store(1, std::memory_order_release);
        }
        else if (existing != key)
            continue;
        while (!slot.ready.load(std::memory_order_acquire))
            YieldProcessor();
        const auto bucket = g_stereoEnabled.load(std::memory_order_relaxed)
            ? static_cast<std::size_t>(1 + (ReadTrueOutput() & 1)) : 0;
        slot.calls[bucket].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_dispatchCallersDropped.fetch_add(1, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE HookDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    if constexpr (ResearchDiagnostics::Enabled) g_frameDispatches.fetch_add(1, std::memory_order_relaxed);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto rva = caller >= g_exeBase && caller < g_exeBase + kRetailImageSize
        ? caller - g_exeBase : 0;
    RecordDispatchCaller(rva, 0, x, y, z);
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    if (hook && hook->dispatch)
        hook->dispatch(context, x, y, z);
}

void RememberProbeWeightResource(ID3D11Resource* resource)
{
    const auto identity = reinterpret_cast<std::uintptr_t>(resource);
    if (!identity)
        return;
    for (auto& slot : g_probeWeightResources)
    {
        auto existing = slot.load(std::memory_order_acquire);
        if (existing == identity)
            return;
        if (existing == 0 && slot.compare_exchange_strong(existing, identity,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
}

bool IsProbeWeightResource(ID3D11Resource* resource)
{
    const auto identity = reinterpret_cast<std::uintptr_t>(resource);
    if (!identity)
        return false;
    for (const auto& slot : g_probeWeightResources)
    {
        if (slot.load(std::memory_order_acquire) == identity)
            return true;
    }
    return false;
}

void RecordProbeConsumer(std::uintptr_t shader, std::uintptr_t resource, std::uint32_t slot)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!shader || !resource)
        return;
    std::uint64_t key = 1469598103934665603ull;
    const std::uint64_t values[] = {shader, resource, slot};
    for (const auto value : values)
        key = (key ^ value) * 1099511628211ull;
    if (key == 0)
        key = 1;
    const auto start = static_cast<std::size_t>((key ^ (key >> 32)) &
        (kProbeConsumerSampleCount - 1));
    for (std::size_t probe = 0; probe < 24; ++probe)
    {
        auto& sample = g_probeConsumers[(start + probe) & (kProbeConsumerSampleCount - 1)];
        auto existing = sample.key.load(std::memory_order_acquire);
        if (existing == 0)
        {
            if (!sample.key.compare_exchange_strong(existing, key,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            sample.shader = shader;
            sample.resource = resource;
            sample.slot = slot;
            sample.ready.store(1, std::memory_order_release);
        }
        else if (existing != key)
            continue;
        while (!sample.ready.load(std::memory_order_acquire))
            YieldProcessor();
        const auto bucket = g_stereoEnabled.load(std::memory_order_relaxed)
            ? static_cast<std::size_t>(1 + (ReadTrueOutput() & 1)) : 0;
        sample.calls[bucket].fetch_add(1, std::memory_order_relaxed);
        if (g_lastProbeWeightResource[bucket].load(std::memory_order_acquire) == resource)
            sample.currentMatches[bucket].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_probeConsumersDropped.fetch_add(1, std::memory_order_relaxed);
}

struct ResourceDescription
{
    D3D11_RESOURCE_DIMENSION dimension{D3D11_RESOURCE_DIMENSION_UNKNOWN};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    UINT width{};
    UINT height{};
    UINT depth{};
    UINT mipLevels{};
    UINT arraySize{};
    UINT sampleCount{};
    UINT bindFlags{};
    UINT miscFlags{};
};

ResourceDescription DescribeResource(ID3D11Resource* resource)
{
    ResourceDescription result{};
    if (!resource)
        return result;
    resource->GetType(&result.dimension);
    switch (result.dimension)
    {
    case D3D11_RESOURCE_DIMENSION_BUFFER:
    {
        D3D11_BUFFER_DESC desc{};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        result.width = desc.ByteWidth;
        result.height = 1;
        result.depth = 1;
        result.mipLevels = 1;
        result.arraySize = 1;
        result.sampleCount = 1;
        result.bindFlags = desc.BindFlags;
        result.miscFlags = desc.MiscFlags;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    {
        D3D11_TEXTURE1D_DESC desc{};
        static_cast<ID3D11Texture1D*>(resource)->GetDesc(&desc);
        result.format = desc.Format;
        result.width = desc.Width;
        result.height = 1;
        result.depth = 1;
        result.mipLevels = desc.MipLevels;
        result.arraySize = desc.ArraySize;
        result.sampleCount = 1;
        result.bindFlags = desc.BindFlags;
        result.miscFlags = desc.MiscFlags;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
        result.format = desc.Format;
        result.width = desc.Width;
        result.height = desc.Height;
        result.depth = 1;
        result.mipLevels = desc.MipLevels;
        result.arraySize = desc.ArraySize;
        result.sampleCount = desc.SampleDesc.Count;
        result.bindFlags = desc.BindFlags;
        result.miscFlags = desc.MiscFlags;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    {
        D3D11_TEXTURE3D_DESC desc{};
        static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
        result.format = desc.Format;
        result.width = desc.Width;
        result.height = desc.Height;
        result.depth = desc.Depth;
        result.mipLevels = desc.MipLevels;
        result.arraySize = 1;
        result.sampleCount = 1;
        result.bindFlags = desc.BindFlags;
        result.miscFlags = desc.MiscFlags;
        break;
    }
    default:
        break;
    }
    return result;
}

void LogAmbientConsumerSnapshot(ID3D11DeviceContext* context, std::size_t bucket,
    std::uintptr_t shaderIdentity, std::uint32_t probeMask,
    ID3D11ShaderResourceView* const* srvs, ID3D11Resource* const* resources)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!context)
        return;
    const auto mode = bucket == 0 ? "mono" : bucket == 1 ? "o0" : "o1";
    Log("[AMBIENT] mode=%s ps=%p probeMask=0x%04X", mode,
        reinterpret_cast<void*>(shaderIdentity), probeMask);
    for (std::size_t index = 0; index < 16; ++index)
    {
        if (!srvs[index] || !resources[index])
            continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        srvs[index]->GetDesc(&viewDesc);
        const auto resourceDesc = DescribeResource(resources[index]);
        Log("[AMBIENT_SRV] mode=%s slot=%u view=%p res=%p vfmt=%u vdim=%u rdim=%u size=%ux%ux%u mip=%u array=%u samples=%u rfmt=%u bind=0x%X misc=0x%X probe=%u",
            mode, static_cast<unsigned>(index), srvs[index], resources[index],
            static_cast<unsigned>(viewDesc.Format),
            static_cast<unsigned>(viewDesc.ViewDimension),
            static_cast<unsigned>(resourceDesc.dimension), resourceDesc.width,
            resourceDesc.height, resourceDesc.depth, resourceDesc.mipLevels,
            resourceDesc.arraySize, resourceDesc.sampleCount,
            static_cast<unsigned>(resourceDesc.format), resourceDesc.bindFlags,
            resourceDesc.miscFlags, (probeMask & (1u << index)) ? 1u : 0u);
    }

    ID3D11Buffer* cbs[8]{};
    context->PSGetConstantBuffers(0, 8, cbs);
    for (std::size_t index = 0; index < 8; ++index)
    {
        if (!cbs[index])
            continue;
        D3D11_BUFFER_DESC desc{};
        cbs[index]->GetDesc(&desc);
        const auto* sample = FindPoseBuffer(cbs[index], 0, false);
        Log("[AMBIENT_CB] mode=%s slot=%u cb=%p size=%u bind=0x%X hash=%016llX writes=%u changes=%u",
            mode, static_cast<unsigned>(index), cbs[index], desc.ByteWidth,
            desc.BindFlags, static_cast<unsigned long long>(sample
                ? sample->lastHash.load(std::memory_order_acquire) : 0),
            sample ? sample->writes.load(std::memory_order_relaxed) : 0,
            sample ? sample->changes.load(std::memory_order_relaxed) : 0);
        cbs[index]->Release();
    }

    ID3D11RenderTargetView* rtvs[8]{};
    ID3D11DepthStencilView* dsv{};
    context->OMGetRenderTargets(8, rtvs, &dsv);
    for (std::size_t index = 0; index < 8; ++index)
    {
        if (!rtvs[index])
            continue;
        ID3D11Resource* resource{};
        rtvs[index]->GetResource(&resource);
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
        rtvs[index]->GetDesc(&viewDesc);
        const auto resourceDesc = DescribeResource(resource);
        Log("[AMBIENT_RTV] mode=%s slot=%u view=%p res=%p vfmt=%u vdim=%u size=%ux%u samples=%u rfmt=%u bind=0x%X",
            mode, static_cast<unsigned>(index), rtvs[index], resource,
            static_cast<unsigned>(viewDesc.Format),
            static_cast<unsigned>(viewDesc.ViewDimension), resourceDesc.width,
            resourceDesc.height, resourceDesc.sampleCount,
            static_cast<unsigned>(resourceDesc.format), resourceDesc.bindFlags);
        if (resource)
            resource->Release();
        rtvs[index]->Release();
    }
    if (dsv)
    {
        ID3D11Resource* resource{};
        dsv->GetResource(&resource);
        D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
        dsv->GetDesc(&viewDesc);
        const auto resourceDesc = DescribeResource(resource);
        Log("[AMBIENT_DSV] mode=%s view=%p res=%p vfmt=%u vdim=%u size=%ux%u samples=%u rfmt=%u bind=0x%X",
            mode, dsv, resource, static_cast<unsigned>(viewDesc.Format),
            static_cast<unsigned>(viewDesc.ViewDimension), resourceDesc.width,
            resourceDesc.height, resourceDesc.sampleCount,
            static_cast<unsigned>(resourceDesc.format), resourceDesc.bindFlags);
        if (resource)
            resource->Release();
        dsv->Release();
    }
}

void RememberMonoAmbientAo(ID3D11ShaderResourceView* view, ID3D11Resource* resource)
{
    if (!view || !resource)
        return;
    const auto desc = DescribeResource(resource);
    if (desc.dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.format != DXGI_FORMAT_R8_UNORM || desc.width <= 1 || desc.height <= 1)
        return;
    view->AddRef();
    ID3D11ShaderResourceView* previous{};
    bool changed{};
    AcquireSRWLockExclusive(&g_monoAmbientAoLock);
    if (g_monoAmbientAoSrv != view)
    {
        previous = g_monoAmbientAoSrv;
        g_monoAmbientAoSrv = view;
        changed = true;
    }
    ReleaseSRWLockExclusive(&g_monoAmbientAoLock);
    if (!changed)
        view->Release();
    if (previous)
        previous->Release();
    if (changed)
    {
        g_monoAmbientAoLearned.fetch_add(1, std::memory_order_relaxed);
        Log("[AOPROOF] learned mono AO view=%p res=%p size=%ux%u fmt=%u",
            view, resource, desc.width, desc.height, static_cast<unsigned>(desc.format));
    }
}

ID3D11ShaderResourceView* AcquireMonoAmbientAo()
{
    ID3D11ShaderResourceView* result{};
    AcquireSRWLockShared(&g_monoAmbientAoLock);
    result = g_monoAmbientAoSrv;
    if (result)
        result->AddRef();
    ReleaseSRWLockShared(&g_monoAmbientAoLock);
    return result;
}

bool TryApplyMonoAmbientAoProof(ID3D11DeviceContext* context, UINT vertices, UINT start,
    ID3D11ShaderResourceView** originalOut, ID3D11ShaderResourceView** replacementOut)
{
    if (originalOut)
        *originalOut = nullptr;
    if (replacementOut)
        *replacementOut = nullptr;
    if constexpr (!kUseMonoAmbientAoProof)
        return false;
    if (!context || vertices != 4 || start != 0 ||
        !g_stereoEnabled.load(std::memory_order_relaxed))
        return false;

    ID3D11ShaderResourceView* views[3]{};
    ID3D11Resource* resources[3]{};
    context->PSGetShaderResources(5, 3, views);
    for (std::size_t index = 0; index < 3; ++index)
    {
        if (views[index])
            views[index]->GetResource(&resources[index]);
    }
    const auto fallback = DescribeResource(resources[0]);
    const bool ambientConsumer = IsProbeWeightResource(resources[2]);
    const bool hasFallbackAo = fallback.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D &&
        fallback.width == 1 && fallback.height == 1;
    ID3D11ShaderResourceView* replacement = ambientConsumer && hasFallbackAo
        ? AcquireMonoAmbientAo() : nullptr;
    bool applied{};
    if (replacement && replacement != views[0])
    {
        context->PSSetShaderResources(5, 1, &replacement);
        if (originalOut)
        {
            *originalOut = views[0];
            views[0] = nullptr;
        }
        if (replacementOut)
        {
            *replacementOut = replacement;
            replacement = nullptr;
        }
        g_monoAmbientAoProofApplied[ReadTrueOutput() & 1].fetch_add(
            1, std::memory_order_relaxed);
        applied = true;
    }
    else if (ambientConsumer && hasFallbackAo)
        g_monoAmbientAoProofMissed.fetch_add(1, std::memory_order_relaxed);
    if (replacement)
        replacement->Release();
    for (auto*& resource : resources)
    {
        if (resource)
            resource->Release();
    }
    for (auto*& view : views)
    {
        if (view)
            view->Release();
    }
    return applied;
}

// [UIFIND] Where does this game draw its interface?
//
// Classifying only INDEXED draws does not work here: the [DRAW4] counter
// shows about four thousand FOUR-VERTEX unindexed Draw calls per second,
// roughly seventy quads a frame, which is what a sprite-batched HUD looks
// like, while indexed draws show a single pixel shader for a whole session.
//
// This census makes no assumption about which call carries the interface.  It
// records every draw entry point, keyed by pixel shader, and gathers the
// evidence that separates interface from world:
//
//   - callsWhileFlat: draws issued while the scene has NO 3D camera at all
//     (a loading screen or a movie).  Only 2D work runs then, so a shader with
//     a high flat count is interface by observation rather than by rule;
//     depth: whether a depth-stencil is bound, which world geometry always has
//     and screen-space work never does;
//   - lateness: how far into the frame the shader draws, since the interface
//     is composited last;
//   - target size and viewport, to tell a full-screen composite from a sprite.
//
// Read-only.  The expensive state queries run once per shader, not per draw.
constexpr std::size_t kDrawFindSlots = 256;


struct DrawFindSlot
{
    std::atomic_uintptr_t ps{};
    std::atomic_uint64_t calls{};
    std::atomic_uint64_t callsWhileFlat{};
    std::atomic_uint32_t lanes{};
    std::atomic_uint32_t lastVertexCount{};
    std::atomic_uint32_t lastDrawIndex{};
    std::atomic_uint32_t probed{};
    std::atomic_uint32_t hasDepth{};
    std::atomic_uint32_t rtWidth{};
    std::atomic_uint32_t rtHeight{};
    std::atomic_uint32_t vpWidth{};
    std::atomic_uint32_t vpHeight{};
    // [UIGATE] Name every shader the interface gate accepts, once each, so a
    // wrong selection is readable in the log instead of only on screen.
    std::atomic_uint32_t gateLogged{};
};

std::array<DrawFindSlot, kDrawFindSlots> g_drawFind{};
std::atomic_uint32_t g_drawFindIndex{};      // draw counter within this frame
std::atomic_uint32_t g_drawFindFrameTotal{}; // previous frame's total
std::atomic_uint32_t g_drawFindDumps{};
std::atomic_uint64_t g_drawFindNoShader{};

// [FRAMECALLS] Neither cached tables (four exist; patching all of them
// changes nothing) nor indirect submission (DrawAuto and both Indirect entry
// points record nothing) explain draws missing from the hooks.  These
// counters answer whether the scene uses the hooked contexts at all: they
// count the constant-buffer uploads the lens scan reads every frame against
// the draws.  Thousands of state calls beside sixty draws means draws are
// dispatched by a route the mod does not own; sixty of everything means the
// scene lives on a device or context the mod has not seen.
// Counters live earlier in the file, next to the hooks that feed them.

void DrawFindProbe(ID3D11DeviceContext* context, DrawFindSlot& slot)
{
    ID3D11RenderTargetView* rtv{};
    ID3D11DepthStencilView* dsv{};
    context->OMGetRenderTargets(1, &rtv, &dsv);
    slot.hasDepth.store(dsv ? 1u : 0u, std::memory_order_relaxed);
    if (rtv)
    {
        ID3D11Resource* resource{};
        rtv->GetResource(&resource);
        if (resource)
        {
            ID3D11Texture2D* texture{};
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&texture))) && texture)
            {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                slot.rtWidth.store(desc.Width, std::memory_order_relaxed);
                slot.rtHeight.store(desc.Height, std::memory_order_relaxed);
                texture->Release();
            }
            resource->Release();
        }
        rtv->Release();
    }
    if (dsv)
        dsv->Release();
    UINT viewportCount = 1;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&viewportCount, &viewport);
    if (viewportCount)
    {
        slot.vpWidth.store(static_cast<std::uint32_t>(viewport.Width),
            std::memory_order_relaxed);
        slot.vpHeight.store(static_cast<std::uint32_t>(viewport.Height),
            std::memory_order_relaxed);
    }
}

void DrawFindRecord(ID3D11DeviceContext* context, UINT vertexCount,
    std::uint32_t lane)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!context)
        return;
    // Count every draw that reaches the hook BEFORE anything can fail, so
    // "the census is empty" and "the hook is not being called" are told apart.
    const auto index = g_drawFindIndex.fetch_add(1, std::memory_order_relaxed);
    ID3D11PixelShader* shader{};
    // PSGetShader must be given a class-instance count even when no instance
    // array is wanted.  Passing nullptr for both returns no shader at all
    // here, which collapses every draw into one signature.
    UINT classInstanceCount = 0;
    context->PSGetShader(&shader, nullptr, &classInstanceCount);
    const auto key = reinterpret_cast<std::uintptr_t>(shader);
    if (shader)
        shader->Release();
    if (!key)
    {
        g_drawFindNoShader.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool flat = RetailXr::GetSceneIsFlat();
    const auto start = (key >> 4) & (kDrawFindSlots - 1);
    for (std::size_t probe = 0; probe < 32; ++probe)
    {
        auto& slot = g_drawFind[(start + probe) & (kDrawFindSlots - 1)];
        auto existing = slot.ps.load(std::memory_order_acquire);
        if (existing == 0 && !slot.ps.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
            existing = slot.ps.load(std::memory_order_acquire);
        if (existing != 0 && existing != key)
            continue;
        slot.calls.fetch_add(1, std::memory_order_relaxed);
        if (flat)
            slot.callsWhileFlat.fetch_add(1, std::memory_order_relaxed);
        slot.lanes.fetch_or(lane, std::memory_order_relaxed);
        slot.lastVertexCount.store(vertexCount, std::memory_order_relaxed);
        slot.lastDrawIndex.store(index, std::memory_order_relaxed);
        std::uint32_t probed = 0;
        if (slot.probed.compare_exchange_strong(probed, 1u,
                std::memory_order_acq_rel, std::memory_order_relaxed))
            DrawFindProbe(context, slot);
        return;
    }
}

const char* DrawFindLaneName(std::uint32_t lanes)
{
    switch (lanes & 0xF)
    {
    case 1: return "Draw";
    case 2: return "DrawIndexed";
    case 4: return "DrawIdxInst";
    case 8: return "DrawInst";
    case 16: return "DrawVtbl";
    case 32: return "DrawAuto";
    case 64: return "IdxIndirect";
    case 128: return "Indirect";
    default: return "mixed";
    }
}

// Called once per present.  Dumps the shaders most likely to be the interface
// first: the ones that keep drawing when there is no 3D camera on screen.
void DrawFindTick()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const auto total = g_drawFindIndex.exchange(0, std::memory_order_acq_rel);
    if (total)
        g_drawFindFrameTotal.store(total, std::memory_order_relaxed);
    {
        static std::uint64_t lastCallLog = 0;
        const auto now = g_presentCount.load(std::memory_order_relaxed);
        const auto vsSetCb = g_frameVsSetCb.exchange(0, std::memory_order_acq_rel);
        const auto maps = g_frameMaps.exchange(0, std::memory_order_acq_rel);
        const auto updates = g_frameUpdates.exchange(0, std::memory_order_acq_rel);
        const auto dispatches =
            g_frameDispatches.exchange(0, std::memory_order_acq_rel);
        if (now - lastCallLog >= 300)
        {
            lastCallLog = now;
            Log("[FRAMECALLS] per frame: draws=%u vsSetCB=%u map=%u "
                "updateSub=%u dispatch=%u slotReasserts=%llu", total, vsSetCb,
                maps, updates, dispatches,
                static_cast<unsigned long long>(g_drawSlotReasserts.exchange(
                    0, std::memory_order_acq_rel)));
        }
    }
    static std::uint64_t lastDump = 0;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    if (present - lastDump < 900 ||
        g_drawFindDumps.load(std::memory_order_relaxed) >= 24)
        return;
    lastDump = present;
    g_drawFindDumps.fetch_add(1, std::memory_order_relaxed);

    struct Entry
    {
        std::uintptr_t ps;
        std::uint64_t calls;
        std::uint64_t flat;
        std::uint32_t lanes;
        std::uint32_t verts;
        std::uint32_t index;
        std::uint32_t depth;
        std::uint32_t rtW;
        std::uint32_t rtH;
        std::uint32_t vpW;
        std::uint32_t vpH;
    };
    std::array<Entry, kDrawFindSlots> entries{};
    std::size_t count = 0;
    for (auto& slot : g_drawFind)
    {
        const auto ps = slot.ps.load(std::memory_order_relaxed);
        if (!ps)
            continue;
        entries[count++] = Entry{ps,
            slot.calls.load(std::memory_order_relaxed),
            slot.callsWhileFlat.load(std::memory_order_relaxed),
            slot.lanes.load(std::memory_order_relaxed),
            slot.lastVertexCount.load(std::memory_order_relaxed),
            slot.lastDrawIndex.load(std::memory_order_relaxed),
            slot.hasDepth.load(std::memory_order_relaxed),
            slot.rtWidth.load(std::memory_order_relaxed),
            slot.rtHeight.load(std::memory_order_relaxed),
            slot.vpWidth.load(std::memory_order_relaxed),
            slot.vpHeight.load(std::memory_order_relaxed)};
    }
    const auto frameTotal = g_drawFindFrameTotal.load(std::memory_order_relaxed);
    Log("[UIFIND] shaders=%zu drawsPerFrame=%u noShader=%llu  (flat = drawn "
        "while no 3D camera; depth=0 and late index are screen-space markers)",
        count, frameTotal,
        static_cast<unsigned long long>(
            g_drawFindNoShader.exchange(0, std::memory_order_acq_rel)));
    const auto dump = [&](const char* title, bool byFlat)
    {
        std::sort(entries.begin(), entries.begin() + count,
            [byFlat](const Entry& a, const Entry& b)
            {
                return byFlat ? a.flat > b.flat : a.calls > b.calls;
            });
        std::size_t shown = 0;
        for (std::size_t i = 0; i < count && shown < 12; ++i)
        {
            const auto& e = entries[i];
            if (byFlat && e.flat == 0)
                break;
            ++shown;
            Log("[UIFIND] %s ps=%llX %s calls=%llu flat=%llu verts=%u "
                "lastIdx=%u/%u depth=%u rt=%ux%u vp=%ux%u",
                title, static_cast<unsigned long long>(e.ps),
                DrawFindLaneName(e.lanes),
                static_cast<unsigned long long>(e.calls),
                static_cast<unsigned long long>(e.flat),
                e.verts, e.index, frameTotal, e.depth,
                e.rtW, e.rtH, e.vpW, e.vpH);
        }
        if (byFlat && shown == 0)
            Log("[UIFIND] %s none yet - no flat frames seen, load a save or "
                "watch a movie so the 2D-only shaders label themselves", title);
    };
    dump("FLAT", true);
    dump("BUSY", false);
    // Arrivals before failures: with the control ON, a fitted count of zero
    // means the gate matched nothing, which is a different failure from the
    // gate matching the wrong draws.
    Log("[UIGATE] control=%s fittedDraws=%llu",
        UiHook::GetFitEnabled() ? "ON" : "off", UiHook::GetFitDrawCount());
}

void ObserveReplayDraw(ID3D11DeviceContext* context, UINT vertices, UINT start)
{
    if constexpr (!ResearchDiagnostics::Enabled && !kUseMonoAmbientAoProof) return;
    if (context && vertices == 4 && start == 0)
    {
        const auto bucket = g_stereoEnabled.load(std::memory_order_relaxed)
            ? static_cast<std::size_t>(1 + (ReadTrueOutput() & 1)) : 0;
        g_draw4Calls[bucket].fetch_add(1, std::memory_order_relaxed);
        ID3D11ShaderResourceView* srvs[16]{};
        ID3D11Resource* resources[16]{};
        context->PSGetShaderResources(0, 16, srvs);
        bool haveProbeWeight{};
        std::uint32_t knownMask{};
        for (std::size_t index = 0; index < 16; ++index)
        {
            if (srvs[index])
                srvs[index]->GetResource(&resources[index]);
            if (IsProbeWeightResource(resources[index]))
            {
                haveProbeWeight = true;
                knownMask |= 1u << index;
            }
        }
        const auto now = GetTickCount64();
        auto previous = g_draw4LastLogMs[bucket].load(std::memory_order_acquire);
        const bool logSample = (previous == 0 || now - previous >= 1000) &&
            g_draw4LastLogMs[bucket].compare_exchange_strong(previous, now,
                std::memory_order_acq_rel, std::memory_order_acquire);
        ID3D11PixelShader* shader{};
        std::uintptr_t shaderIdentity{};
        if (haveProbeWeight || logSample)
        {
            UINT classInstanceCount{};
            context->PSGetShader(&shader, nullptr, &classInstanceCount);
            shaderIdentity = reinterpret_cast<std::uintptr_t>(shader);
        }
        if (haveProbeWeight)
        {
            if (bucket == 0 && srvs[5] && resources[5])
                RememberMonoAmbientAo(srvs[5], resources[5]);
            for (std::size_t index = 0; index < 16; ++index)
            {
                if (IsProbeWeightResource(resources[index]))
                    RecordProbeConsumer(shaderIdentity,
                        reinterpret_cast<std::uintptr_t>(resources[index]),
                        static_cast<std::uint32_t>(index));
            }
            auto previousSnapshot = g_ambientSnapshotLastLogMs[bucket].load(
                std::memory_order_acquire);
            if ((previousSnapshot == 0 || now - previousSnapshot >= 1000) &&
                g_ambientSnapshotLastLogMs[bucket].compare_exchange_strong(
                    previousSnapshot, now, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                LogAmbientConsumerSnapshot(context, bucket, shaderIdentity, knownMask,
                    srvs, resources);
        }
        if (logSample)
            Log("[DRAW4] mode=%s ps=%p known=0x%04X res=%p/%p/%p/%p/%p/%p/%p/%p/%p/%p/%p/%p/%p/%p/%p/%p",
                bucket == 0 ? "mono" : bucket == 1 ? "o0" : "o1",
                reinterpret_cast<void*>(shaderIdentity), knownMask,
                resources[0], resources[1], resources[2], resources[3],
                resources[4], resources[5], resources[6], resources[7],
                resources[8], resources[9], resources[10], resources[11],
                resources[12], resources[13], resources[14], resources[15]);
        if (shader)
            shader->Release();
        for (auto*& resource : resources)
        {
            if (resource)
                resource->Release();
        }
        for (auto*& view : srvs)
        {
            if (view)
                view->Release();
        }
    }
}

void STDMETHODCALLTYPE HookReplayDraw(ID3D11DeviceContext* context, UINT vertices, UINT start)
{
    ObserveReplayDraw(context, vertices, start);
    DrawFindRecord(context, vertices, 16u);
    auto* hook = context
        ? FindContextVtableHook(*reinterpret_cast<void***>(context)) : nullptr;
    if (hook && hook->draw)
        hook->draw(context, vertices, start);
}

// [UICEN] one-eye draw census.  Observed: world-space UI (markers,
// damage numbers, enemy HP, lock-on) lands in output 1 ONLY; screen-space HUD
// is in both.  Both eyes share ONE render target in SBS and differ by
// VIEWPORT, so a one-eyed signature + right-half viewport means the fix is a
// same-draw replay with the left-half viewport.  This census keys draws by
// pixel-shader pointer across the three indexed/instanced draw entry points
// (the working hook layer; the D3D SceneCensus module proved blind) and
// records per-eye call counts plus the viewport it saw.
struct UiCensusSlot
{
    std::atomic_uintptr_t ps{};
    std::atomic<std::uint64_t> calls[3]{};
    std::atomic<std::uint32_t> vpX{};
    std::atomic<std::uint32_t> vpW{};
    std::atomic<std::uint32_t> lastIndexCount{};
    // [UIFIT] 0 = not classified yet, 1 = user interface, 2 = not interface.
    std::atomic<std::uint32_t> uiVerdict{};
    std::atomic<std::uint32_t> rtWidth{};
    std::atomic<std::uint32_t> rtHeight{};
};
constexpr std::size_t kUiCensusSlots = 512;
std::array<UiCensusSlot, kUiCensusSlots> g_uiCensus{};
std::atomic<std::uint64_t> g_uiCensusDropped{};

// [UIFIT] Bring the interface into the part of the headset you can actually
// look at.  The game draws its HUD to the edges of a flat 16:9 screen, and in
// a headset those edges are past the comfortable field of view, so the corners
// are unreadable without turning your head.
//
// The interface is baked into the same backbuffer as the world, so individual
// elements cannot be moved the way a HUD hook would move them.  What can be
// done is to shrink the RASTER VIEWPORT for interface draws only, which pulls
// everything that is drawn in screen space toward the middle without touching
// a shader or the world.
//
// Classification is cached per pixel shader, because doing it per draw would
// cost several COM round trips in the hottest path in the frame.  An interface
// draw is an INDEXED draw with no depth-stencil bound into a large
// single-sample target; full-screen post-process passes are unindexed, and
// world geometry always carries depth.
std::atomic_bool g_uiFitEnabled{};
std::atomic<float> g_uiFitScaleX{0.85f};
std::atomic<float> g_uiFitScaleY{0.85f};
std::atomic<float> g_uiFitOffsetX{0.0f};
std::atomic<float> g_uiFitOffsetY{0.0f};
std::atomic_uint64_t g_uiFitDraws{};
std::atomic_uint32_t g_uiFitLogs{};

void UiCensusRecord(ID3D11DeviceContext* context, UINT indexCount)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!context)
        return;
    const auto bucket = g_stereoEnabled.load(std::memory_order_relaxed)
        ? static_cast<std::size_t>(1 + (ReadTrueOutput() & 1)) : 0;
    ID3D11PixelShader* shader{};
    // PSGetShader must be given a class-instance count even when no
    // instance array is wanted.  Passing nullptr for both returned no
    // shader at all here, which is what left every interface census in
    // this file reporting one signature while thousands of draws a
    // second went past.
    UINT classInstanceCount = 0;
    context->PSGetShader(&shader, nullptr, &classInstanceCount);
    const auto key = reinterpret_cast<std::uintptr_t>(shader);
    if (shader)
        shader->Release();
    if (!key)
        return;
    const auto start = (key >> 4) & (kUiCensusSlots - 1);
    for (std::size_t probe = 0; probe < 32; ++probe)
    {
        auto& slot = g_uiCensus[(start + probe) & (kUiCensusSlots - 1)];
        auto existing = slot.ps.load(std::memory_order_acquire);
        if (existing == 0 && !slot.ps.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
            existing = slot.ps.load(std::memory_order_acquire);
        if (existing != 0 && existing != key)
            continue;
        slot.calls[bucket].fetch_add(1, std::memory_order_relaxed);
        slot.lastIndexCount.store(indexCount, std::memory_order_relaxed);
        UINT viewportCount = 1;
        D3D11_VIEWPORT viewport{};
        context->RSGetViewports(&viewportCount, &viewport);
        if (viewportCount)
        {
            slot.vpX.store(static_cast<std::uint32_t>(viewport.TopLeftX),
                std::memory_order_relaxed);
            slot.vpW.store(static_cast<std::uint32_t>(viewport.Width),
                std::memory_order_relaxed);
        }
        return;
    }
    g_uiCensusDropped.fetch_add(1, std::memory_order_relaxed);
}

void ProbeBoundViewBuffer(ID3D11DeviceContext* context, UINT indexCount)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!context)
        return;
    const auto mode = g_stereoEnabled.load(std::memory_order_relaxed)
        ? static_cast<std::size_t>(1 + (ReadTrueOutput() & 1)) : 0;
    if (g_viewCbBoundLogs[mode].load(std::memory_order_relaxed) >= 16)
        return;

    ID3D11Buffer* buffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
    context->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT,
        buffers);
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++slot)
    {
        auto* buffer = buffers[slot];
        if (!buffer)
            continue;
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        if (desc.ByteWidth != 352)
            continue;
        const auto logIndex =
            g_viewCbBoundLogs[mode].fetch_add(1, std::memory_order_relaxed);
        if (logIndex >= 16)
            continue;

        auto* sample = FindPoseBuffer(buffer, 0, false);
        ID3D11VertexShader* vertexShader{};
        ID3D11PixelShader* pixelShader{};
        context->VSGetShader(&vertexShader, nullptr, nullptr);
        UINT pixelClassInstances = 0;
        context->PSGetShader(&pixelShader, nullptr, &pixelClassInstances);
        UINT viewportCount = 1;
        D3D11_VIEWPORT viewport{};
        context->RSGetViewports(&viewportCount, &viewport);
        Log("[VIEWCB_BOUND] mode=%s stage=VS slot=%u resource=%p idx=%u "
            "vs=%p ps=%p vp=%.0f/%.0f hash=%016llX writes=%u changes=%u "
            "map=+0x%llX unmap=+0x%llX",
            mode == 0 ? "mono" : mode == 1 ? "o0" : "o1", slot, buffer, indexCount,
            vertexShader, pixelShader, viewport.TopLeftX, viewport.Width,
            static_cast<unsigned long long>(sample
                ? sample->lastHash.load(std::memory_order_relaxed) : 0),
            sample ? sample->writes.load(std::memory_order_relaxed) : 0,
            sample ? sample->changes.load(std::memory_order_relaxed) : 0,
            static_cast<unsigned long long>(sample
                ? sample->producerMapCallerRva.load(std::memory_order_relaxed) : 0),
            static_cast<unsigned long long>(sample
                ? sample->producerUnmapCallerRva.load(std::memory_order_relaxed) : 0));
        if (vertexShader)
            vertexShader->Release();
        if (pixelShader)
            pixelShader->Release();
    }
    for (auto*& buffer : buffers)
    {
        if (buffer)
            buffer->Release();
    }
}

void LogUiCensus()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    std::uint32_t total{}, oneEyed{}, shown{};
    for (auto& slot : g_uiCensus)
    {
        if (!slot.ps.load(std::memory_order_acquire))
            continue;
        ++total;
        const auto o0 = slot.calls[1].load(std::memory_order_relaxed);
        const auto o1 = slot.calls[2].load(std::memory_order_relaxed);
        if ((o0 == 0) == (o1 == 0))
            continue;
        ++oneEyed;
        if ((o0 + o1) < 30 || shown >= 16)
            continue;
        ++shown;
        Log("[UICEN] ONE-EYED ps=%p calls m/o0/o1=%llu/%llu/%llu vpX=%u vpW=%u idx=%u",
            reinterpret_cast<void*>(slot.ps.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(slot.calls[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(o0),
            static_cast<unsigned long long>(o1),
            slot.vpX.load(std::memory_order_relaxed),
            slot.vpW.load(std::memory_order_relaxed),
            slot.lastIndexCount.load(std::memory_order_relaxed));
    }
    Log("[UICEN] signatures=%u oneEyed=%u shown=%u dropped=%llu", total, oneEyed, shown,
        static_cast<unsigned long long>(g_uiCensusDropped.load(std::memory_order_relaxed)));
}

// The engine reads the immediate context's vtable ONCE at init and calls D3D
// through its own cached pointer table (dispatcher +0x2DB0200: CALL [RAX+slot*8]
// - +0x148 == the known [DISPATCH] caller, which validates the decoding).
// That cache is why vtable patches are blind here.  So the CACHE is hooked:
// scan RW memory for five known d3d11 implementation pointers at their slot
// offsets (0x60 DrawIndexed, 0x68 Draw, 0xA0 DrawIndexedInstanced,
// 0xA8 DrawInstanced, 0x148 Dispatch) and swap the draw entries for the
// census hooks.  Pure data patch; original impls stay reachable via the
// saved vtable originals.
std::atomic<std::uintptr_t> g_engineD3dTable{};

// [D3DTABLES] The engine keeps several cached pointer tables.  Patching only
// the FIRST one reaches just the post-processing chain (unindexed
// four-vertex quads into a downsample pyramid, 40-76 draws a frame) while
// a 4K frame is thousands of draws; unpatched tables still hold the
// original pointers.
//
// Patch EVERY table that matches, and keep re-scanning, because tables built
// later by a subsystem that had not run yet would otherwise never be caught.
constexpr std::size_t kEngineD3dTableSlots = 64;
std::array<std::atomic<std::uintptr_t>, kEngineD3dTableSlots> g_engineD3dTables{};
std::atomic_uint32_t g_engineD3dTableCount{};
std::atomic_uint32_t g_engineD3dScans{};
DrawIndexedFn g_realEngineDrawIndexed{};
DrawIndexedInstancedFn g_realEngineDrawIndexedInstanced{};
DrawInstancedFn g_realEngineDrawInstanced{};
// [INDIRECT] The indirect draw entry points: DrawAuto (38),
// DrawIndexedInstancedIndirect (39) and DrawInstancedIndirect (40).  An
// engine that leans on compute this heavily can build draw arguments on the
// GPU and submit them indirectly, which the DIRECT entry points never see.
DrawAutoFn g_realDrawAuto{};
DrawIndirectFn g_realDrawIndexedInstancedIndirect{};
DrawIndirectFn g_realDrawInstancedIndirect{};

void STDMETHODCALLTYPE HookDrawAutoCensus(ID3D11DeviceContext* context)
{
    DrawFindRecord(context, 0, 32u);
    if (g_realDrawAuto)
        g_realDrawAuto(context);
}

void STDMETHODCALLTYPE HookDrawIndexedInstancedIndirectCensus(
    ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    DrawFindRecord(context, 0, 64u);
    if (g_realDrawIndexedInstancedIndirect)
        g_realDrawIndexedInstancedIndirect(context, args, offset);
}

void STDMETHODCALLTYPE HookDrawInstancedIndirectCensus(
    ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    DrawFindRecord(context, 0, 128u);
    if (g_realDrawInstancedIndirect)
        g_realDrawInstancedIndirect(context, args, offset);
}

void STDMETHODCALLTYPE HookDrawIndexedCensus(ID3D11DeviceContext* context, UINT indexCount,
    UINT startIndex, INT baseVertex)
{
    ProbeBoundViewBuffer(context, indexCount);
    UiCensusRecord(context, indexCount);
    DrawFindRecord(context, indexCount, 2u);
    if (g_realEngineDrawIndexed)
        g_realEngineDrawIndexed(context, indexCount, startIndex, baseVertex);
    else
    {
        auto* hook = context
            ? FindContextVtableHook(*reinterpret_cast<void***>(context)) : nullptr;
        if (hook && hook->drawIndexed)
            hook->drawIndexed(context, indexCount, startIndex, baseVertex);
    }
}

void STDMETHODCALLTYPE HookDrawIndexedInstancedCensus(ID3D11DeviceContext* context,
    UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex,
    UINT startInstance)
{
    UiCensusRecord(context, indexCountPerInstance);
    DrawFindRecord(context, indexCountPerInstance, 4u);
    if (g_realEngineDrawIndexedInstanced)
        g_realEngineDrawIndexedInstanced(context, indexCountPerInstance, instanceCount,
            startIndex, baseVertex, startInstance);
    else
    {
        auto* hook = context
            ? FindContextVtableHook(*reinterpret_cast<void***>(context)) : nullptr;
        if (hook && hook->drawIndexedInstanced)
            hook->drawIndexedInstanced(context, indexCountPerInstance, instanceCount,
                startIndex, baseVertex, startInstance);
    }
}

void STDMETHODCALLTYPE HookDrawInstancedCensus(ID3D11DeviceContext* context,
    UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance)
{
    UiCensusRecord(context, vertexCountPerInstance);
    DrawFindRecord(context, vertexCountPerInstance, 8u);
    if (g_realEngineDrawInstanced)
    {
        g_realEngineDrawInstanced(context, vertexCountPerInstance, instanceCount,
            startVertex, startInstance);
        return;
    }
    auto* hook = context
        ? FindContextVtableHook(*reinterpret_cast<void***>(context)) : nullptr;
    if (hook && hook->drawInstanced)
        hook->drawInstanced(context, vertexCountPerInstance, instanceCount,
            startVertex, startInstance);
}

// [TABLECHECK] 4700 VSSetConstantBuffers calls per frame against 58 draws:
// the game DOES use these contexts.  VSSetConstantBuffers lives at 0x38 of
// the same cached tables whose 0x60 (DrawIndexed) is silent, so the engine
// rebuilds or refreshes its cache and reverts the draw patch silently.
//
// Read every patched slot back, report what is actually in it, and restore any
// that reverted.
void VerifyEngineD3dTables()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const auto claimed = std::min<std::uint32_t>(
        g_engineD3dTableCount.load(std::memory_order_acquire),
        static_cast<std::uint32_t>(kEngineD3dTableSlots));
    if (!claimed)
        return;
    struct SlotCheck
    {
        std::size_t offset;
        void* hook;
        const char* name;
    };
    const SlotCheck checks[] = {
        {0x38, reinterpret_cast<void*>(&HookVSSetConstantBuffers), "VSSetCB"},
        {0x60, reinterpret_cast<void*>(&HookDrawIndexedCensus), "DrawIndexed"},
        {0xA0, reinterpret_cast<void*>(&HookDrawIndexedInstancedCensus),
            "DrawIdxInst"},
        {0xA8, reinterpret_cast<void*>(&HookDrawInstancedCensus), "DrawInst"},
    };
    for (std::uint32_t index = 0; index < claimed; ++index)
    {
        const auto address =
            g_engineD3dTables[index].load(std::memory_order_relaxed);
        if (!address)
            continue;
        __try
        {
            auto* table = reinterpret_cast<std::uintptr_t*>(address);
            char line[320];
            int written = std::snprintf(line, sizeof(line),
                "[TABLECHECK] table %u at %p:", index + 1,
                reinterpret_cast<void*>(address));
            std::uint32_t restored = 0;
            for (const auto& check : checks)
            {
                const bool ours = table[check.offset / 8] ==
                    reinterpret_cast<std::uintptr_t>(check.hook);
                if (written > 0 && written < static_cast<int>(sizeof(line)) - 40)
                    written += std::snprintf(line + written,
                        sizeof(line) - static_cast<std::size_t>(written),
                        " %s=%s", check.name, ours ? "ours" : "REVERTED");
                if (!ours)
                {
                    table[check.offset / 8] =
                        reinterpret_cast<std::uintptr_t>(check.hook);
                    ++restored;
                }
            }
            if (restored && written > 0 &&
                written < static_cast<int>(sizeof(line)) - 24)
                std::snprintf(line + written,
                    sizeof(line) - static_cast<std::size_t>(written),
                    " (re-patched %u)", restored);
            static std::atomic_uint32_t verifyLogs{};
            if (restored ||
                verifyLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("%s", line);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

bool TryHookEngineD3dTable()
{
    if constexpr (!ResearchDiagnostics::Enabled) return false;
    // The signature values: the ORIGINAL d3d11 implementation pointers
    // saved before patching the vtable.  Any hooked slot would hold the mod's
    // function instead, so read the saved originals.
    ContextVtableHook* source{};
    for (auto& hook : g_contextVtableHooks)
    {
        if (hook.table.load(std::memory_order_acquire) && hook.drawIndexed &&
            hook.draw && hook.dispatch)
        {
            source = &hook;
            break;
        }
    }
    if (!source)
        return false;
    const auto drawIndexedImpl = reinterpret_cast<std::uintptr_t>(source->drawIndexed);
    const auto drawImpl = reinterpret_cast<std::uintptr_t>(source->draw);
    const auto drawIndexedInstancedImpl =
        reinterpret_cast<std::uintptr_t>(source->drawIndexedInstanced);
    const auto drawInstancedImpl = reinterpret_cast<std::uintptr_t>(source->drawInstanced);
    const auto dispatchImpl = reinterpret_cast<std::uintptr_t>(source->dispatch);
    if (!drawIndexedImpl || !drawImpl || !dispatchImpl)
        return false;

    const auto d3dModule = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"d3d11.dll"));
    const auto selfModule = reinterpret_cast<std::uintptr_t>(g_self);
    std::uint32_t patched = 0;

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    auto address = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maxAddress = reinterpret_cast<std::uintptr_t>(
        systemInfo.lpMaximumApplicationAddress);
    while (address < maxAddress)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)))
            break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto regionEnd = regionBase + info.RegionSize;
        const bool writable = info.State == MEM_COMMIT &&
            (info.Protect == PAGE_READWRITE || info.Protect == PAGE_EXECUTE_READWRITE);
        const auto allocationBase = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
        if (writable && allocationBase != d3dModule && allocationBase != selfModule &&
            info.RegionSize >= 0x150)
        {
            __try
            {
                const auto last = regionEnd - 0x150;
                for (auto candidate = regionBase; candidate <= last; candidate += 8)
                {
                    const auto* qwords = reinterpret_cast<const std::uintptr_t*>(candidate);
                    if (qwords[0x60 / 8] != drawIndexedImpl ||
                        qwords[0x68 / 8] != drawImpl ||
                        qwords[0x148 / 8] != dispatchImpl)
                        continue;
                    auto* table = reinterpret_cast<std::uintptr_t*>(candidate);
                    // Already patched on an earlier scan?
                    bool known = false;
                    const auto claimed =
                        g_engineD3dTableCount.load(std::memory_order_acquire);
                    for (std::uint32_t i = 0; i < claimed &&
                        i < kEngineD3dTableSlots; ++i)
                        if (g_engineD3dTables[i].load(std::memory_order_relaxed) ==
                            candidate)
                        {
                            known = true;
                            break;
                        }
                    if (known)
                        continue;
                    g_realEngineVsSetConstantBuffers = source->vsSetConstantBuffers;
                    g_realEngineDrawIndexed = source->drawIndexed;
                    g_realEngineDrawIndexedInstanced = source->drawIndexedInstanced;
                    g_realEngineDrawInstanced = source->drawInstanced;
                    table[0x38 / 8] =
                        reinterpret_cast<std::uintptr_t>(&HookVSSetConstantBuffers);
                    table[0x60 / 8] = reinterpret_cast<std::uintptr_t>(&HookDrawIndexedCensus);
                    if (qwords[0xA0 / 8] == drawIndexedInstancedImpl)
                        table[0xA0 / 8] =
                            reinterpret_cast<std::uintptr_t>(&HookDrawIndexedInstancedCensus);
                    if (qwords[0xA8 / 8] == drawInstancedImpl)
                        table[0xA8 / 8] =
                            reinterpret_cast<std::uintptr_t>(&HookDrawInstancedCensus);
                    if (g_realDrawAuto && qwords[0x130 / 8] ==
                        reinterpret_cast<std::uintptr_t>(g_realDrawAuto))
                        table[0x130 / 8] =
                            reinterpret_cast<std::uintptr_t>(&HookDrawAutoCensus);
                    if (g_realDrawIndexedInstancedIndirect &&
                        qwords[0x138 / 8] == reinterpret_cast<std::uintptr_t>(
                            g_realDrawIndexedInstancedIndirect))
                        table[0x138 / 8] = reinterpret_cast<std::uintptr_t>(
                            &HookDrawIndexedInstancedIndirectCensus);
                    if (g_realDrawInstancedIndirect &&
                        qwords[0x140 / 8] == reinterpret_cast<std::uintptr_t>(
                            g_realDrawInstancedIndirect))
                        table[0x140 / 8] = reinterpret_cast<std::uintptr_t>(
                            &HookDrawInstancedIndirectCensus);
                    g_engineD3dTable.store(candidate, std::memory_order_release);
                    const auto slot = g_engineD3dTableCount.fetch_add(1,
                        std::memory_order_acq_rel);
                    if (slot < kEngineD3dTableSlots)
                        g_engineD3dTables[slot].store(candidate,
                            std::memory_order_release);
                    ++patched;
                    Log("[D3DTABLES] table %u at %p patched (VSSetCB, DrawIndexed, "
                        "DrawIndexedInstanced, DrawInstanced)",
                        slot + 1, reinterpret_cast<void*>(candidate));
                    // Keep scanning: this engine keeps more than one.
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        address = regionEnd;
    }
    const auto scans = g_engineD3dScans.fetch_add(1, std::memory_order_relaxed) + 1;
    if (patched || scans <= 2)
        Log("[D3DTABLES] scan %u patched %u new table(s); %u total",
            scans, patched,
            g_engineD3dTableCount.load(std::memory_order_relaxed));
    return patched != 0;
}

extern "C" void STDMETHODCALLTYPE RetailReplayDrawPolicy(
    ID3D11DeviceContext* context, UINT vertices, UINT start)
{
    ObserveReplayDraw(context, vertices, start);
    if (!context)
        return;
    // [UIFIND] This is the draw path this engine actually uses.  The context
    // vtable hook next to it sees about one call a frame; this one carries
    // thousands a second.
    DrawFindRecord(context, vertices, 1u);
    // [LATEHOOK] And this is the context that DRAWS.  The context seen by the
    // VSSetConstantBuffers hook issues twelve thousand state calls a frame
    // and zero draws; the drawing context passes through here, four
    // thousand times a second, and hands itself over directly.  Once per
    // vtable; the installer is idempotent and keeps its originals per vtable.
    {
        const auto drawVtable = *reinterpret_cast<std::uintptr_t*>(context);
        if (ResearchDiagnostics::Enabled && DrawSlotVerdict(drawVtable) == 1)
        {
            SceneCensus::EnsureContextHooked(context);
            MarkDrawSlotOffered(drawVtable);
        }
    }
    ID3D11ShaderResourceView* originalAo{};
    ID3D11ShaderResourceView* replacementAo{};
    const bool aoProof = TryApplyMonoAmbientAoProof(context, vertices, start,
        &originalAo, &replacementAo);
    // The interface fit lives in ui_hook.cpp now, on the d3d11.dll draw
    // implementations, which every draw in the process passes through.
    auto** table = *reinterpret_cast<void***>(context);
    const auto draw = table ? reinterpret_cast<DrawFn>(table[13]) : nullptr;
    if (draw)
        draw(context, vertices, start);
    if (aoProof)
    {
        context->PSSetShaderResources(5, 1, &originalAo);
        if (originalAo)
            originalAo->Release();
        if (replacementAo)
            replacementAo->Release();
    }
}

extern "C" void STDMETHODCALLTYPE RetailReplayDispatchPolicy(
    ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    std::uintptr_t shaderIdentity{};
    ID3D11ComputeShader* shader{};
    if (ResearchDiagnostics::Enabled && context)
    {
        UINT classInstanceCount{};
        context->CSGetShader(&shader, nullptr, &classInstanceCount);
        shaderIdentity = reinterpret_cast<std::uintptr_t>(shader);
    }
    RecordDispatchCaller(kReplayDispatchCallRva, shaderIdentity, x, y, z);
    ID3D11UnorderedAccessView* probeUav{};
    ID3D11Resource* probeWeightResource{};
    if ((ResearchDiagnostics::Enabled || kUseMonoAmbientAoProof) &&
        context && x == 320 && y == 180 && z == 1)
    {
        const auto bucket = g_stereoEnabled.load(std::memory_order_relaxed)
            ? static_cast<std::size_t>(1 + (ReadTrueOutput() & 1)) : 0;
        context->CSGetUnorderedAccessViews(0, 1, &probeUav);
        if (probeUav)
            probeUav->GetResource(&probeWeightResource);
        RememberProbeWeightResource(probeWeightResource);
        g_lastProbeWeightResource[bucket].store(
            reinterpret_cast<std::uintptr_t>(probeWeightResource), std::memory_order_release);
        const auto now = GetTickCount64();
        auto previous = g_probeResourceLastLogMs[bucket].load(std::memory_order_acquire);
        if ((previous == 0 || now - previous >= 1000) &&
            g_probeResourceLastLogMs[bucket].compare_exchange_strong(previous, now,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            ID3D11ShaderResourceView* srvs[3]{};
            ID3D11Buffer* cbs[4]{};
            ID3D11Resource* resources[3]{};
            context->CSGetShaderResources(0, 3, srvs);
            context->CSGetConstantBuffers(0, 4, cbs);
            for (std::size_t index = 0; index < 3; ++index)
            {
                if (srvs[index])
                    srvs[index]->GetResource(&resources[index]);
            }
            Log("[PROBERES] mode=%s shader=%p srv=%p/%p/%p res=%p/%p/%p uav=%p res=%p cb=%p/%p/%p/%p",
                bucket == 0 ? "mono" : bucket == 1 ? "o0" : "o1",
                reinterpret_cast<void*>(shaderIdentity), srvs[0], srvs[1], srvs[2],
                resources[0], resources[1], resources[2], probeUav, probeWeightResource,
                cbs[0], cbs[1], cbs[2], cbs[3]);
            auto* cbSample = FindPoseBuffer(cbs[1], 0, false);
            Log("[PROBECB] mode=%s cb=%p size=%u bind=0x%X hash=%016llX writes=%u changes=%u writer=+0x%llX",
                bucket == 0 ? "mono" : bucket == 1 ? "o0" : "o1", cbs[1],
                cbSample ? cbSample->byteWidth : 0, cbSample ? cbSample->bindFlags : 0,
                static_cast<unsigned long long>(cbSample
                    ? cbSample->lastHash.load(std::memory_order_acquire) : 0),
                cbSample ? cbSample->writes.load(std::memory_order_relaxed) : 0,
                cbSample ? cbSample->changes.load(std::memory_order_relaxed) : 0,
                static_cast<unsigned long long>(cbSample
                    ? cbSample->sourceWriterRva.load(std::memory_order_acquire) : 0));
            for (auto*& resource : resources)
            {
                if (resource)
                    resource->Release();
            }
            for (auto*& view : srvs)
            {
                if (view)
                    view->Release();
            }
            for (auto*& cb : cbs)
            {
                if (cb)
                    cb->Release();
            }
        }
    }
    if (probeWeightResource)
        probeWeightResource->Release();
    if (probeUav)
        probeUav->Release();
    if (shader)
        shader->Release();
    if (!context)
        return;
    auto** currentTable = *reinterpret_cast<void***>(context);
    if (currentTable && !FindContextVtableHook(currentTable))
        HookContextVtable(context);
    auto** table = *reinterpret_cast<void***>(context);
    auto dispatch = table ? reinterpret_cast<DispatchFn>(table[41]) : nullptr;
    if (dispatch)
        dispatch(context, x, y, z);
}

HRESULT STDMETHODCALLTYPE HookFinishCommandList(ID3D11DeviceContext* context,
    BOOL restoreDeferredContextState, ID3D11CommandList** commandList)
{
    auto* hook = FindContextVtableHook(*reinterpret_cast<void***>(context));
    const HRESULT result = hook && hook->finish
        ? hook->finish(context, restoreDeferredContextState, commandList) : E_FAIL;
    if (SUCCEEDED(result) && commandList && *commandList)
    {
        g_commandListsFinished.fetch_add(1, std::memory_order_relaxed);
    }
    else
        g_commandListsFinishFailed.fetch_add(1, std::memory_order_relaxed);
    return result;
}

// [VTWATCH] Log-only watchdog for the Meta runtime's known habit of replacing
// or rewriting D3D11 context vtables when its XR session starts. Every ~2 s
// it compares the game immediate context's live table and the patched slots
// against what was installed, and logs each change once. It repairs nothing.
void WatchContextVtables(IDXGISwapChain* swapChain)
{
    static unsigned presents = 0;
    static void** reportedTable = nullptr;
    static unsigned reportedMask = 0;
    if ((++presents % 240) != 0 || !swapChain)
        return;
    ID3D11Device* device{};
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device)
        return;
    ID3D11DeviceContext* context{};
    device->GetImmediateContext(&context);
    device->Release();
    if (!context)
        return;
    void** live = *reinterpret_cast<void***>(context);
    context->Release();
    if (!live)
        return;
    const bool known = FindContextVtableHook(live) != nullptr;
    struct Slot { int index; void* ours; const char* name; };
    const Slot slots[] = {
        {14, reinterpret_cast<void*>(&HookMap), "Map"},
        {15, reinterpret_cast<void*>(&HookUnmap), "Unmap"},
        {48, reinterpret_cast<void*>(&HookUpdateSubresource), "UpdateSubresource"},
        {7, reinterpret_cast<void*>(&HookVSSetConstantBuffers), "VSSetConstantBuffers"},
        {13, reinterpret_cast<void*>(&HookReplayDraw), "Draw"},
        {12, reinterpret_cast<void*>(&HookDrawIndexedCensus), "DrawIndexed"},
        {47, reinterpret_cast<void*>(&HookCopyResource), "CopyResource"},
        {58, reinterpret_cast<void*>(&HookExecuteCommandList), "ExecuteCommandList"},
    };
    unsigned mask = 0;
    for (unsigned i = 0; i < _countof(slots); ++i)
        if (live[slots[i].index] != slots[i].ours)
            mask |= 1u << i;
    if (live == reportedTable && mask == reportedMask)
        return;
    if (!reportedTable)
        Log("[VTWATCH] armed: game context vtable=%p known=%d evictedMask=0x%X", live, known ? 1 : 0, mask);
    else
        Log("[VTWATCH] CHANGED: game context vtable %p -> %p known=%d evictedMask 0x%X -> 0x%X",
            reportedTable, live, known ? 1 : 0, reportedMask, mask);
    for (unsigned i = 0; i < _countof(slots); ++i)
        if (mask & (1u << i))
            Log("[VTWATCH]   slot %d %s no longer points at the mod's hook (now %p)",
                slots[i].index, slots[i].name, live[slots[i].index]);
    reportedTable = live;
    reportedMask = mask;
}

bool HookContextVtable(ID3D11DeviceContext* context)
{
    if (!context)
        return false;
    void** table = *reinterpret_cast<void***>(context);
    if (!table)
        return false;

    AcquireSRWLockExclusive(&g_contextHookLock);
    if (FindContextVtableHook(table))
    {
        ReleaseSRWLockExclusive(&g_contextHookLock);
        return true;
    }

    ContextVtableHook* selected{};
    for (auto& hook : g_contextVtableHooks)
    {
        if (!hook.table.load(std::memory_order_acquire))
        {
            selected = &hook;
            break;
        }
    }
    if (!selected)
    {
        ReleaseSRWLockExclusive(&g_contextHookLock);
        return false;
    }

    selected->execute = reinterpret_cast<ExecuteCommandListFn>(table[58]);
    selected->finish = reinterpret_cast<FinishCommandListFn>(table[114]);
    selected->map = reinterpret_cast<MapFn>(table[14]);
    selected->unmap = reinterpret_cast<UnmapFn>(table[15]);
    selected->update = reinterpret_cast<UpdateSubresourceFn>(table[48]);
    selected->dispatch = reinterpret_cast<DispatchFn>(table[41]);
    selected->vsSetConstantBuffers = reinterpret_cast<VSSetConstantBuffersFn>(table[7]);
    selected->draw = reinterpret_cast<DrawFn>(table[13]);
    selected->table.store(table, std::memory_order_release);
    const bool mapOk = PatchVtable(table, 14, reinterpret_cast<void*>(&HookMap),
        reinterpret_cast<void**>(&selected->map));
    const bool unmapOk = PatchVtable(table, 15, reinterpret_cast<void*>(&HookUnmap),
        reinterpret_cast<void**>(&selected->unmap));
    const bool updateOk = PatchVtable(table, 48, reinterpret_cast<void*>(&HookUpdateSubresource),
        reinterpret_cast<void**>(&selected->update));
    const bool dispatchOk = PatchVtable(table, 41, reinterpret_cast<void*>(&HookDispatch),
        reinterpret_cast<void**>(&selected->dispatch));
    const bool vsCbOk = PatchVtable(table, 7,
        reinterpret_cast<void*>(&HookVSSetConstantBuffers),
        reinterpret_cast<void**>(&selected->vsSetConstantBuffers));
    const bool drawOk = PatchVtable(table, 13, reinterpret_cast<void*>(&HookReplayDraw),
        reinterpret_cast<void**>(&selected->draw));
    selected->drawIndexed = reinterpret_cast<DrawIndexedFn>(table[12]);
    selected->drawIndexedInstanced = reinterpret_cast<DrawIndexedInstancedFn>(table[20]);
    selected->drawInstanced = reinterpret_cast<DrawInstancedFn>(table[21]);
    selected->copyResource = reinterpret_cast<CopyResourceFn>(table[47]);
    selected->copySubresourceRegion =
        reinterpret_cast<CopySubresourceRegionFn>(table[46]);
    PatchVtable(table, 47, reinterpret_cast<void*>(&HookCopyResource),
        reinterpret_cast<void**>(&selected->copyResource));
    PatchVtable(table, 46, reinterpret_cast<void*>(&HookCopySubresourceRegion),
        reinterpret_cast<void**>(&selected->copySubresourceRegion));
    // [INDIRECT] Same d3d11 implementations on every context, so one saved
    // original each is correct.
    if (!g_realDrawAuto)
    {
        g_realDrawAuto = reinterpret_cast<DrawAutoFn>(table[38]);
        g_realDrawIndexedInstancedIndirect =
            reinterpret_cast<DrawIndirectFn>(table[39]);
        g_realDrawInstancedIndirect =
            reinterpret_cast<DrawIndirectFn>(table[40]);
    }
    void* discarded = nullptr;
    PatchVtable(table, 38, reinterpret_cast<void*>(&HookDrawAutoCensus),
        &discarded);
    PatchVtable(table, 39,
        reinterpret_cast<void*>(&HookDrawIndexedInstancedIndirectCensus),
        &discarded);
    PatchVtable(table, 40,
        reinterpret_cast<void*>(&HookDrawInstancedIndirectCensus), &discarded);
    PatchVtable(table, 12, reinterpret_cast<void*>(&HookDrawIndexedCensus),
        reinterpret_cast<void**>(&selected->drawIndexed));
    PatchVtable(table, 20, reinterpret_cast<void*>(&HookDrawIndexedInstancedCensus),
        reinterpret_cast<void**>(&selected->drawIndexedInstanced));
    PatchVtable(table, 21, reinterpret_cast<void*>(&HookDrawInstancedCensus),
        reinterpret_cast<void**>(&selected->drawInstanced));
    const bool executeOk = PatchVtable(table, 58, reinterpret_cast<void*>(&HookExecuteCommandList),
        reinterpret_cast<void**>(&selected->execute));
    const bool finishOk = PatchVtable(table, 114, reinterpret_cast<void*>(&HookFinishCommandList),
        reinterpret_cast<void**>(&selected->finish));
    ID3D11DeviceContext1* context1{};
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
            reinterpret_cast<void**>(&context1))) && context1)
    {
        PatchVtable(table, 115,
            reinterpret_cast<void*>(&HookCopySubresourceRegion1),
            reinterpret_cast<void**>(&selected->copySubresourceRegion1));
        PatchVtable(table, 116,
            reinterpret_cast<void*>(&HookUpdateSubresource1),
            reinterpret_cast<void**>(&selected->updateSubresource1));
        PatchVtable(table, 119,
            reinterpret_cast<void*>(&HookVSSetConstantBuffers1),
            reinterpret_cast<void**>(&selected->vsSetConstantBuffers1));
        context1->Release();
    }
    if (executeOk || finishOk)
        g_contextVtablesHooked.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_contextHookLock);

    Log("[LEAK] context vtable=%p hooks map=%d unmap=%d update=%d dispatch=%d vsCB=%d draw=%d execute=%d finish=%d", table,
        mapOk ? 1 : 0, unmapOk ? 1 : 0, updateOk ? 1 : 0,
        dispatchOk ? 1 : 0, vsCbOk ? 1 : 0, drawOk ? 1 : 0, executeOk ? 1 : 0,
        finishOk ? 1 : 0);
    return mapOk && unmapOk && updateOk && dispatchOk && drawOk && executeOk && finishOk;
}

void InstallCommandListProbe(IDXGISwapChain* swapChain)
{
    if (!swapChain)
        return;
    bool expected = false;
    if (!g_commandProbeInstalled.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    ID3D11Device* device{};
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device),
            reinterpret_cast<void**>(&device))) || !device)
    {
        g_commandProbeInstalled.store(false, std::memory_order_release);
        return;
    }

    ID3D11DeviceContext* immediate{};
    if constexpr (ResearchDiagnostics::Enabled) InstallResourceProbe(device);
    device->GetImmediateContext(&immediate);
    ID3D11DeviceContext* deferred{};
    const HRESULT deferredResult = device->CreateDeferredContext(0, &deferred);
    // [D3DHOOK] The inline hooks on the d3d11.dll implementations go in
    // FIRST, while these vtables still hold the real entry points.  Everything
    // below patches vtable slots and would otherwise hand back its own detours.
    UiHook::Install(device, immediate, deferred, &Log, &ReadTrueOutput);
    const bool immediateOk = HookContextVtable(immediate);
    const bool deferredOk = SUCCEEDED(deferredResult) && HookContextVtable(deferred);
    SceneCensus::Install(device, immediate, deferred, &Log, &ReadTrueOutput);
    if (immediate)
        immediate->Release();
    if (deferred)
        deferred->Release();
    device->Release();

    Log("[LEAK] command-list probe installed immediate=%d deferred=%d deferredHr=0x%08lX",
        immediateOk ? 1 : 0, deferredOk ? 1 : 0,
        static_cast<unsigned long>(deferredResult));
}

void MaybeLogLeakTelemetry()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart <= 0)
        return;
    const auto previousTick = g_lastTelemetryTicks.load(std::memory_order_acquire);
    if (previousTick != 0 && now.QuadPart - previousTick < frequency.QuadPart)
        return;
    auto expectedTick = previousTick;
    if (!g_lastTelemetryTicks.compare_exchange_strong(expectedTick, now.QuadPart,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    using GetMemoryInfoFn = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static const auto getMemoryInfo = reinterpret_cast<GetMemoryInfoFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!getMemoryInfo || !getMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&memory), sizeof(memory)))
        return;

    DWORD handles{};
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const auto finished = g_commandListsFinished.load(std::memory_order_relaxed);
    const auto executed = g_commandListsExecuted.load(std::memory_order_relaxed);
    const auto failed = g_commandListsFinishFailed.load(std::memory_order_relaxed);
    const auto deltaPrivate = g_lastPrivateBytes == 0
        ? 0ll : static_cast<long long>(memory.PrivateUsage) -
            static_cast<long long>(g_lastPrivateBytes);
    const auto deltaFinished = finished - g_lastFinished;
    const auto deltaExecuted = executed - g_lastExecuted;
    const auto pending = static_cast<long long>(finished) - static_cast<long long>(executed);
    constexpr std::uint64_t kMegabyte = 1024ull * 1024ull;

    DXGI_QUERY_VIDEO_MEMORY_INFO gpuMemory{};
    const bool haveGpuMemory = g_leakAdapter && SUCCEEDED(g_leakAdapter->QueryVideoMemoryInfo(
        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &gpuMemory));
    const auto buffersCreated = g_buffersCreated.load(std::memory_order_relaxed);
    const auto buffersDestroyed = g_buffersDestroyed.load(std::memory_order_relaxed);
    const auto bufferBytesCreated = g_bufferBytesCreated.load(std::memory_order_relaxed);
    const auto bufferBytesDestroyed = g_bufferBytesDestroyed.load(std::memory_order_relaxed);
    const auto texturesCreated = g_texturesCreated.load(std::memory_order_relaxed);
    const auto texturesDestroyed = g_texturesDestroyed.load(std::memory_order_relaxed);
    const auto textureBytesCreated = g_textureBytesCreated.load(std::memory_order_relaxed);
    const auto textureBytesDestroyed = g_textureBytesDestroyed.load(std::memory_order_relaxed);

    Log("[LEAK] mem private=%lluMB delta=%lldMB ws=%lluMB handles=%lu stereo=%d "
        "gpu=%llu/%lluMB cmd(finish=%llu +%llu execute=%llu +%llu pending=%lld fail=%llu contexts=%llu) "
        "res(buf=%llu/%llu live=%lld est=%lldMB tex=%llu/%llu live=%lld est=%lldMB)",
        static_cast<unsigned long long>(memory.PrivateUsage / kMegabyte),
        static_cast<long long>(deltaPrivate / static_cast<long long>(kMegabyte)),
        static_cast<unsigned long long>(memory.WorkingSetSize / kMegabyte),
        static_cast<unsigned long>(handles),
        g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(haveGpuMemory ? gpuMemory.CurrentUsage / kMegabyte : 0),
        static_cast<unsigned long long>(haveGpuMemory ? gpuMemory.Budget / kMegabyte : 0),
        static_cast<unsigned long long>(finished),
        static_cast<unsigned long long>(deltaFinished),
        static_cast<unsigned long long>(executed),
        static_cast<unsigned long long>(deltaExecuted),
        pending,
        static_cast<unsigned long long>(failed),
        static_cast<unsigned long long>(g_contextVtablesHooked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(buffersCreated),
        static_cast<unsigned long long>(buffersDestroyed),
        static_cast<long long>(buffersCreated) - static_cast<long long>(buffersDestroyed),
        static_cast<long long>((bufferBytesCreated - bufferBytesDestroyed) / kMegabyte),
        static_cast<unsigned long long>(texturesCreated),
        static_cast<unsigned long long>(texturesDestroyed),
        static_cast<long long>(texturesCreated) - static_cast<long long>(texturesDestroyed),
        static_cast<long long>((textureBytesCreated - textureBytesDestroyed) / kMegabyte));

    const bool stereoForTelemetry = g_stereoEnabled.load(std::memory_order_acquire);
    LogPoseBufferTelemetry(stereoForTelemetry);
    LogVsConstantBufferCandidates(stereoForTelemetry);
    LogPassNameTelemetry();
    LogOutputCallerTelemetry();
    LogDispatchCallerTelemetry();
    LogProbeConsumerTelemetry();
    const auto deferredTrue = g_policyTrueDeferred.exchange(0, std::memory_order_acq_rel);
    if (deferredTrue)
        Log("[OUTPOLICY] deferredManagerTrue=%llu",
            static_cast<unsigned long long>(deferredTrue));
    const auto displayAreaBankTrue = g_policyTrueDisplayAreaBank.exchange(
        0, std::memory_order_acq_rel);
    if (displayAreaBankTrue)
        Log("[OUTPOLICY] displayAreaBankTrue=%llu",
            static_cast<unsigned long long>(displayAreaBankTrue));

    const auto lodMono = g_actorLodBudgetMonoCalls.exchange(0, std::memory_order_acq_rel);
    const auto lodStereo = g_actorLodBudgetStereoCalls.exchange(0, std::memory_order_acq_rel);
    std::array<std::uint64_t, 2> lodOutputs{};
    std::array<std::uint64_t, 8> lodGroups{};
    std::array<std::uint64_t, 8> lodInputs{};
    std::array<std::uint64_t, 8> lodResults{};
    std::array<std::uint64_t, 8> lodDistances{};
    for (std::size_t index = 0; index < lodOutputs.size(); ++index)
        lodOutputs[index] = g_actorLodBudgetOutputCalls[index].exchange(
            0, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < lodGroups.size(); ++index)
    {
        lodGroups[index] = g_actorLodBudgetGroupCalls[index].exchange(
            0, std::memory_order_acq_rel);
        lodInputs[index] = g_actorLodBudgetInputLevelCalls[index].exchange(
            0, std::memory_order_acq_rel);
        lodResults[index] = g_actorLodBudgetResultLevelCalls[index].exchange(
            0, std::memory_order_acq_rel);
        const auto monoDistance = g_actorLodDistanceBins[0][index].exchange(
            0, std::memory_order_acq_rel);
        const auto stereoDistance = g_actorLodDistanceBins[1][index].exchange(
            0, std::memory_order_acq_rel);
        lodDistances[index] = g_stereoEnabled.load(std::memory_order_relaxed)
            ? stereoDistance : monoDistance;
    }
    Log("[ACTORLOD] stereo=%d calls(mono/stereo)=%llu/%llu output=%llu/%llu "
        "group=%llu/%llu/%llu/%llu input=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
        "result=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
        "distanceBins=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
        g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(lodMono),
        static_cast<unsigned long long>(lodStereo),
        static_cast<unsigned long long>(lodOutputs[0]),
        static_cast<unsigned long long>(lodOutputs[1]),
        static_cast<unsigned long long>(lodGroups[0]),
        static_cast<unsigned long long>(lodGroups[1]),
        static_cast<unsigned long long>(lodGroups[2]),
        static_cast<unsigned long long>(lodGroups[3]),
        static_cast<unsigned long long>(lodInputs[0]),
        static_cast<unsigned long long>(lodInputs[1]),
        static_cast<unsigned long long>(lodInputs[2]),
        static_cast<unsigned long long>(lodInputs[3]),
        static_cast<unsigned long long>(lodInputs[4]),
        static_cast<unsigned long long>(lodInputs[5]),
        static_cast<unsigned long long>(lodInputs[6]),
        static_cast<unsigned long long>(lodInputs[7]),
        static_cast<unsigned long long>(lodResults[0]),
        static_cast<unsigned long long>(lodResults[1]),
        static_cast<unsigned long long>(lodResults[2]),
        static_cast<unsigned long long>(lodResults[3]),
        static_cast<unsigned long long>(lodResults[4]),
        static_cast<unsigned long long>(lodResults[5]),
        static_cast<unsigned long long>(lodResults[6]),
        static_cast<unsigned long long>(lodResults[7]),
        static_cast<unsigned long long>(lodDistances[0]),
        static_cast<unsigned long long>(lodDistances[1]),
        static_cast<unsigned long long>(lodDistances[2]),
        static_cast<unsigned long long>(lodDistances[3]),
        static_cast<unsigned long long>(lodDistances[4]),
        static_cast<unsigned long long>(lodDistances[5]),
        static_cast<unsigned long long>(lodDistances[6]),
        static_cast<unsigned long long>(lodDistances[7]));

    const auto pointFallbackMonoZero = g_actorPointFallbackZero[0].exchange(
        0, std::memory_order_acq_rel);
    const auto pointFallbackMonoSet = g_actorPointFallbackSet[0].exchange(
        0, std::memory_order_acq_rel);
    const auto pointFallbackStereoZero = g_actorPointFallbackZero[1].exchange(
        0, std::memory_order_acq_rel);
    const auto pointFallbackStereoSet = g_actorPointFallbackSet[1].exchange(
        0, std::memory_order_acq_rel);
    const auto pointStateMonoZero = g_actorPointStateZero[0].exchange(
        0, std::memory_order_acq_rel);
    const auto pointStateMonoSet = g_actorPointStateSet[0].exchange(
        0, std::memory_order_acq_rel);
    const auto pointStateStereoZero = g_actorPointStateZero[1].exchange(
        0, std::memory_order_acq_rel);
    const auto pointStateStereoSet = g_actorPointStateSet[1].exchange(
        0, std::memory_order_acq_rel);
    Log("[LODPOINT] stereo=%d fallback(mono0/set stereo0/set)=%llu/%llu %llu/%llu "
        "state44(mono0/set stereo0/set)=%llu/%llu %llu/%llu",
        g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(pointFallbackMonoZero),
        static_cast<unsigned long long>(pointFallbackMonoSet),
        static_cast<unsigned long long>(pointFallbackStereoZero),
        static_cast<unsigned long long>(pointFallbackStereoSet),
        static_cast<unsigned long long>(pointStateMonoZero),
        static_cast<unsigned long long>(pointStateMonoSet),
        static_cast<unsigned long long>(pointStateStereoZero),
        static_cast<unsigned long long>(pointStateStereoSet));

    std::uint64_t boundedFallback[2][2]{};
    std::uint64_t boundedState34[2][2]{};
    std::uint64_t boundedState36[2][2]{};
    for (std::size_t mode = 0; mode < 2; ++mode)
    {
        boundedFallback[mode][0] = g_actorBoundedFallbackZero[mode].exchange(
            0, std::memory_order_acq_rel);
        boundedFallback[mode][1] = g_actorBoundedFallbackSet[mode].exchange(
            0, std::memory_order_acq_rel);
        boundedState34[mode][0] = g_actorBoundedState34Zero[mode].exchange(
            0, std::memory_order_acq_rel);
        boundedState34[mode][1] = g_actorBoundedState34Set[mode].exchange(
            0, std::memory_order_acq_rel);
        boundedState36[mode][0] = g_actorBoundedState36Zero[mode].exchange(
            0, std::memory_order_acq_rel);
        boundedState36[mode][1] = g_actorBoundedState36Set[mode].exchange(
            0, std::memory_order_acq_rel);
    }
    const auto boundedForced = g_actorBoundedFallbackForced.exchange(
        0, std::memory_order_acq_rel);
    const auto boundedAnimationForced = g_actorBoundedAnimationLodForced.exchange(
        0, std::memory_order_acq_rel);
    Log("[LODBOUND] stereo=%d fallback(m0/set s0/set)=%llu/%llu %llu/%llu "
        "state34=%llu/%llu %llu/%llu state36=%llu/%llu %llu/%llu "
        "collisionForced=%llu animationLodForced=%llu ackForced=%llu",
        g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(boundedFallback[0][0]),
        static_cast<unsigned long long>(boundedFallback[0][1]),
        static_cast<unsigned long long>(boundedFallback[1][0]),
        static_cast<unsigned long long>(boundedFallback[1][1]),
        static_cast<unsigned long long>(boundedState34[0][0]),
        static_cast<unsigned long long>(boundedState34[0][1]),
        static_cast<unsigned long long>(boundedState34[1][0]),
        static_cast<unsigned long long>(boundedState34[1][1]),
        static_cast<unsigned long long>(boundedState36[0][0]),
        static_cast<unsigned long long>(boundedState36[0][1]),
        static_cast<unsigned long long>(boundedState36[1][0]),
        static_cast<unsigned long long>(boundedState36[1][1]),
        static_cast<unsigned long long>(boundedForced),
        static_cast<unsigned long long>(boundedAnimationForced),
        static_cast<unsigned long long>(
            g_actorAckForced.exchange(0, std::memory_order_relaxed)));

    for (std::size_t mode = 0; mode < 2; ++mode)
    {
        for (std::size_t base = 0; base < 0x40; base += 8)
        {
            std::uint64_t zero[8]{}, one[8]{}, other[8]{};
            for (std::size_t index = 0; index < 8; ++index)
            {
                zero[index] = g_actorBoundedByteZero[mode][base + index].exchange(
                    0, std::memory_order_acq_rel);
                one[index] = g_actorBoundedByteOne[mode][base + index].exchange(
                    0, std::memory_order_acq_rel);
                other[index] = g_actorBoundedByteOther[mode][base + index].exchange(
                    0, std::memory_order_acq_rel);
            }
            if (zero[0] + one[0] + other[0] == 0)
                continue;
            Log("[LODBYTES] mode=%s base=0x%02zX "
                "0=%llu/%llu/%llu 1=%llu/%llu/%llu 2=%llu/%llu/%llu "
                "3=%llu/%llu/%llu 4=%llu/%llu/%llu 5=%llu/%llu/%llu "
                "6=%llu/%llu/%llu 7=%llu/%llu/%llu",
                mode == 0 ? "mono" : "stereo", base,
                static_cast<unsigned long long>(zero[0]), static_cast<unsigned long long>(one[0]), static_cast<unsigned long long>(other[0]),
                static_cast<unsigned long long>(zero[1]), static_cast<unsigned long long>(one[1]), static_cast<unsigned long long>(other[1]),
                static_cast<unsigned long long>(zero[2]), static_cast<unsigned long long>(one[2]), static_cast<unsigned long long>(other[2]),
                static_cast<unsigned long long>(zero[3]), static_cast<unsigned long long>(one[3]), static_cast<unsigned long long>(other[3]),
                static_cast<unsigned long long>(zero[4]), static_cast<unsigned long long>(one[4]), static_cast<unsigned long long>(other[4]),
                static_cast<unsigned long long>(zero[5]), static_cast<unsigned long long>(one[5]), static_cast<unsigned long long>(other[5]),
                static_cast<unsigned long long>(zero[6]), static_cast<unsigned long long>(one[6]), static_cast<unsigned long long>(other[6]),
                static_cast<unsigned long long>(zero[7]), static_cast<unsigned long long>(one[7]), static_cast<unsigned long long>(other[7]));
        }
    }

    for (auto& caller : g_actorLodCallers)
    {
        const auto rva = caller.rva.load(std::memory_order_acquire);
        if (!rva)
            continue;
        std::array<std::uint64_t, 8> monoLevels{};
        std::array<std::uint64_t, 8> stereoLevels{};
        std::uint64_t total{};
        for (std::size_t level = 0; level < 8; ++level)
        {
            monoLevels[level] = caller.levels[0][level].exchange(
                0, std::memory_order_acq_rel);
            stereoLevels[level] = caller.levels[1][level].exchange(
                0, std::memory_order_acq_rel);
            total += monoLevels[level] + stereoLevels[level];
        }
        if (!total)
            continue;
        Log("[LODCALLER] caller=+0x%llX mono=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
            "stereo=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
            static_cast<unsigned long long>(rva),
            static_cast<unsigned long long>(monoLevels[0]),
            static_cast<unsigned long long>(monoLevels[1]),
            static_cast<unsigned long long>(monoLevels[2]),
            static_cast<unsigned long long>(monoLevels[3]),
            static_cast<unsigned long long>(monoLevels[4]),
            static_cast<unsigned long long>(monoLevels[5]),
            static_cast<unsigned long long>(monoLevels[6]),
            static_cast<unsigned long long>(monoLevels[7]),
            static_cast<unsigned long long>(stereoLevels[0]),
            static_cast<unsigned long long>(stereoLevels[1]),
            static_cast<unsigned long long>(stereoLevels[2]),
            static_cast<unsigned long long>(stereoLevels[3]),
            static_cast<unsigned long long>(stereoLevels[4]),
            static_cast<unsigned long long>(stereoLevels[5]),
            static_cast<unsigned long long>(stereoLevels[6]),
            static_cast<unsigned long long>(stereoLevels[7]));
    }

    g_lastPrivateBytes = memory.PrivateUsage;
    g_lastFinished = finished;
    g_lastExecuted = executed;
}

int* ResolveCurrentOutputCell()
{
    if (!g_exeBase)
        return nullptr;
    __try
    {
        const std::uint32_t tlsIndex = *reinterpret_cast<const std::uint32_t*>(g_exeBase + kTlsIndexRva);
        auto** tlsArray = reinterpret_cast<std::uint8_t**>(__readgsqword(0x58));
        std::uint8_t* slot = tlsArray ? tlsArray[tlsIndex] : nullptr;
        std::uint8_t* context = slot ? *reinterpret_cast<std::uint8_t**>(slot + 0x10) : nullptr;
        if (!context)
            context = reinterpret_cast<std::uint8_t*>(g_exeBase + kFallbackRenderContextRva);
        return reinterpret_cast<int*>(context + kCurrentOutputOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

int ReadTrueOutput()
{
    __try
    {
        const int* cell = ResolveCurrentOutputCell();
        const int output = cell ? *cell : 0;
        return output == 1 ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

void RecordSepProbeEvent(std::uint32_t type, const float* matrix)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!g_sepProbeEventRecording.load(std::memory_order_acquire))
        return;
    const auto generation = g_sepProbeEventGeneration.load(std::memory_order_acquire);
    const auto index = g_sepProbeEventNext.fetch_add(1, std::memory_order_acq_rel);
    if (index >= g_sepProbeEvents.size())
        return;
    auto& event = g_sepProbeEvents[index];
    event.ready.store(false, std::memory_order_relaxed);
    event.generation = generation;
    event.jobSequence = g_sepProbeActiveJobSequence;
    event.present = g_presentCount.load(std::memory_order_relaxed);
    event.camera = g_sepProbeActiveCamera;
    event.threadId = GetCurrentThreadId();
    event.output = ReadTrueOutput();
    event.type = type;
    event.separation = g_stereoSeparationAddress
        ? *g_stereoSeparationAddress : -1.0f;
    event.baseline = g_stereoBaselineAddress
        ? *g_stereoBaselineAddress : -1.0f;
    std::memset(event.matrix, 0, sizeof(event.matrix));
    if (matrix && IsReadable(matrix, sizeof(event.matrix)))
    {
        __try
        {
            std::memcpy(event.matrix, matrix, sizeof(event.matrix));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            std::memset(event.matrix, 0, sizeof(event.matrix));
        }
    }
    if (g_sepProbeEventRecording.load(std::memory_order_acquire) &&
        generation == g_sepProbeEventGeneration.load(std::memory_order_acquire))
        event.ready.store(true, std::memory_order_release);
}

// [CAMSRC] read-only probe of the camera-source lane: dump the camera object
// the job receives, the `source` argument the eye builders read, and the
// stereo object, one shot per arm (F9 and each F10 press re-arm).  Purpose:
// locate the world-space camera position fields so a later change can inject
// a real per-pass eye translation at the source instead of patching buffers.
// Camera-object FOV scalar at +0x300.  The source+0x40 staging block is a
// dead copy nothing renders from, so it cannot show whether +0x300 works; the
// render-side evidence (copies=0, no CPU writes ever reaching the
// always-bound 8192-byte CBs) says the real projection is composed GPU-SIDE
// from CPU parameters, and an upstream parameter change (the title-asset FOV)
// visibly works.  +0x304/+0x308 are display copies.  Driven by the Insert menu
// override; idempotent per game rewrite; forced rewrite on slider change;
// restored on toggle-off and stereo-off.
constexpr std::ptrdiff_t kCameraFovScalarOffset = 0x300;
std::atomic<float> g_fovLastWrittenScalar{};
std::atomic<float> g_fovOriginalScalar{};
std::atomic<float> g_fovLastFactor{1.0f};
std::atomic_uintptr_t g_fovCameraObject{};

void ApplyGameFovScale(void* camera)
{
    if (!kEnableCameraScalarLane || !camera ||
        !g_liveFovOverrideEnabled.load(std::memory_order_acquire))
        return;
    const float target = g_liveFovDegrees.load(std::memory_order_acquire);
    if (!(target > 10.0f) || !(target < 179.0f))
        return;
    constexpr float kPi = 3.14159265358979323846f;
    const float factor = std::tan(kIvFovReferenceDegrees * 0.5f * kPi / 180.0f) /
        std::tan(target * 0.5f * kPi / 180.0f);
    auto* scalar = reinterpret_cast<float*>(
        static_cast<std::uint8_t*>(camera) + kCameraFovScalarOffset);
    if (!IsReadable(scalar, 4))
        return;
    __try
    {
        const float current = *scalar;
        if (!(current == current) || current < 0.05f || current > 10.0f)
            return;
        g_fovCameraObject.store(reinterpret_cast<std::uintptr_t>(camera),
            std::memory_order_relaxed);
        const float lastWritten =
            g_fovLastWrittenScalar.load(std::memory_order_relaxed);
        const float lastFactor =
            g_fovLastFactor.load(std::memory_order_relaxed);
        float original = current;
        const bool gameRewrote = current != lastWritten;
        if (!gameRewrote)
        {
            if (factor == lastFactor)
                return;
            original = g_fovOriginalScalar.load(std::memory_order_relaxed);
        }
        const float scaled = original * factor;
        g_fovOriginalScalar.store(original, std::memory_order_relaxed);
        *scalar = scaled;
        g_fovLastWrittenScalar.store(scaled, std::memory_order_relaxed);
        g_fovLastFactor.store(factor, std::memory_order_relaxed);
        const auto applies =
            g_fovScaleApplies.fetch_add(1, std::memory_order_relaxed) + 1;
        if (applies <= 4 || factor != lastFactor || (applies % 600) == 0)
            Log("[FOVLIVE] camera=%p scalar %.4f -> %.4f factor=%.3f "
                "target=%.1f applies=%llu",
                camera, original, scaled, factor, target,
                static_cast<unsigned long long>(applies));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void RestoreGameFov()
{
    const auto cameraValue = g_fovCameraObject.exchange(0,
        std::memory_order_acq_rel);
    if (!cameraValue)
        return;
    auto* scalar = reinterpret_cast<float*>(cameraValue +
        kCameraFovScalarOffset);
    if (!IsReadable(scalar, 4))
        return;
    __try
    {
        if (*scalar == g_fovLastWrittenScalar.load(std::memory_order_relaxed))
            *scalar = g_fovOriginalScalar.load(std::memory_order_relaxed);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_fovLastWrittenScalar.store(0.0f, std::memory_order_relaxed);
}

std::atomic_uint64_t g_camDumpGeneration{};
std::atomic_uint64_t g_camObjDumpedGeneration{};
std::atomic_uint64_t g_camSrcDumpedGeneration[2]{};
std::atomic_uint64_t g_camStereoDumpedGeneration{};

void DumpProbeBytes(const char* tag, std::uint64_t generation, const void* base,
    std::size_t bytes)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!base || !IsReadable(base, bytes))
    {
        Log("[%s] gen=%llu base=%p UNREADABLE", tag,
            static_cast<unsigned long long>(generation), base);
        return;
    }
    __try
    {
        const auto* words = static_cast<const std::uint32_t*>(base);
        for (std::size_t offset = 0; offset + 16 <= bytes; offset += 16)
        {
            std::uint32_t row[4]{};
            float values[4]{};
            std::memcpy(row, words + offset / 4, 16);
            std::memcpy(values, row, 16);
            Log("[%s] gen=%llu off=%03zX %08X/%08X/%08X/%08X(%g,%g,%g,%g)",
                tag, static_cast<unsigned long long>(generation), offset,
                row[0], row[1], row[2], row[3],
                values[0], values[1], values[2], values[3]);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[%s] gen=%llu base=%p FAULTED mid-dump", tag,
            static_cast<unsigned long long>(generation), base);
    }
}

// [FOVSINK] Does `value` encode `degrees` in any of the forms an engine
// stores a field of view in?
FovSinkKind ClassifyFovEncoding(float value, float degrees)
{
    if (!(value == value) || !(value - value == 0.0f))
        return FovSinkKind::None;
    constexpr float kPi = 3.14159265358979323846f;
    const float radians = degrees * kPi / 180.0f;
    if (std::fabs(value - degrees) < 0.35f)
        return FovSinkKind::Degrees;
    if (std::fabs(value - radians) < 0.006f)
        return FovSinkKind::Radians;
    const float half = radians * 0.5f;
    const float tangent = std::tan(half);
    if (tangent > 1e-4f)
    {
        if (std::fabs(value - 1.0f / tangent) < 0.012f)
            return FovSinkKind::CotHalf;
        if (std::fabs(value - tangent) < 0.012f)
            return FovSinkKind::TanHalf;
    }
    return FovSinkKind::None;
}

const char* FovSinkKindName(FovSinkKind kind)
{
    switch (kind)
    {
    case FovSinkKind::Degrees: return "degrees";
    case FovSinkKind::Radians: return "radians";
    case FovSinkKind::CotHalf: return "cot(fov/2)";
    case FovSinkKind::TanHalf: return "tan(fov/2)";
    default: return "none";
    }
}

// [FOVSINK] Scan the camera object for fields that follow the slider.  A field
// only becomes a candidate after it matches at two or more DISTINCT slider
// values, so a constant that happens to sit near one target cannot qualify.
void ProbeFovSink(void* camera)
{
    if (!camera)
        return;
    if (!g_liveFovOverrideEnabled.load(std::memory_order_acquire) &&
        !VrCameraEnabled())
        return;
    if (g_camParamFovIndex.load(std::memory_order_acquire) < 0)
        return;
    if (!IsReadable(camera, kFovSinkScanBytes))
        return;
    const float degrees = g_liveFovDegrees.load(std::memory_order_acquire);
    if (!(degrees > 10.0f) || !(degrees < 179.0f))
        return;
    g_fovSinkCamera.store(reinterpret_cast<std::uintptr_t>(camera),
        std::memory_order_relaxed);
    g_fovSinkProbes.fetch_add(1, std::memory_order_relaxed);
    __try
    {
        const auto* fields = static_cast<const float*>(camera);
        for (std::uint32_t slot = 0; slot < kFovSinkSlots; ++slot)
        {
            const auto kind = ClassifyFovEncoding(fields[slot], degrees);
            if (kind == FovSinkKind::None)
                continue;
            auto& cell = g_fovSinkCells[slot];
            const auto previous = cell.lastTarget.load(std::memory_order_relaxed);
            if (std::fabs(previous - degrees) < 0.5f)
                continue;
            cell.lastTarget.store(degrees, std::memory_order_relaxed);
            cell.kind.store(static_cast<std::uint8_t>(kind),
                std::memory_order_relaxed);
            const auto matches =
                cell.matches.fetch_add(1, std::memory_order_relaxed) + 1;
            if (matches < 2)
                continue;
            if (g_fovSinkLogs.fetch_add(1, std::memory_order_relaxed) < 24)
                Log("[FOVSINK] candidate camera+0x%X as %s (matched %u distinct "
                    "targets, latest %.1f)",
                    slot * 4, FovSinkKindName(kind), matches, degrees);
            if (g_fovSinkOffset.load(std::memory_order_relaxed) < 0)
            {
                g_fovSinkOffset.store(static_cast<int>(slot * 4),
                    std::memory_order_release);
                g_fovSinkKind.store(static_cast<std::uint8_t>(kind),
                    std::memory_order_release);
                Log("[FOVSINK] SINK SELECTED camera+0x%X encoding=%s",
                    slot * 4, FovSinkKindName(kind));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// [FOVSINK] Write the slider into the discovered sink on every camera job, so
// first person and cutscenes are overridden at the same seat gameplay uses.
void DriveFovSink(void* camera)
{
    // This is the widescreen-mod pattern: one write at a choke point
    // downstream of per-mode authorship, so cutscenes and the game's own
    // first person get the same lens as gameplay.  Runs whenever AER is on
    // and the sink is known; the sink is discovered by the oracle probe and
    // persisted, so later sessions drive it from the first frame.
    if (!camera)
        return;
    // camera+0x2C0 (the oracle probe's find) is a mirror; the real state is
    // the channel table below.  See [FOVCHAN].
    // [NATIVEFOV] Native stereo needs the same pinned lens as AER (without
    // it native stereo stays at the stock 48 degrees).
    if (!VrCameraEnabled() &&
        !g_camParamAllModes.load(std::memory_order_acquire))
        return;
    const float degrees = g_liveFovDegrees.load(std::memory_order_acquire);
    if (!(degrees > 10.0f) || !(degrees < 179.0f))
        return;
    constexpr float kPi = 3.14159265358979323846f;
    const float radians = degrees * kPi / 180.0f;
    // The camera-job object does not hold the channel table (zero writes in
    // the field test); the true manager arrives in RCX at the setter and is
    // captured by the [FOVSET] hook.
    const auto manager = g_fovSetManager.load(std::memory_order_acquire);
    auto* bytes = manager ? reinterpret_cast<std::uint8_t*>(manager)
        : static_cast<std::uint8_t*>(camera);
    if (!IsWritable(bytes + kFovChanBase,
            kFovChanStride * kFovChanCount + sizeof(void*)))
        return;
    __try
    {
        std::uint32_t written = 0;
        float previous = 0.0f;
        for (std::uint32_t channel = 0; channel < kFovChanCount; ++channel)
        {
            auto* value = reinterpret_cast<float*>(
                bytes + kFovChanBase + channel * kFovChanStride + kFovChanValue);
            const float current = *value;
            // Only replace values that look like a live lens; untouched
            // channels hold zero and are left alone.
            if (!(current > 0.05f) || !(current < 3.1f))
                continue;
            if (channel == 0)
                previous = current;
            if (std::fabs(current - radians) > 1e-6f)
            {
                *value = radians;
                ++written;
            }
        }
        if (written)
        {
            const auto count = g_fovChanWrites.fetch_add(1,
                std::memory_order_relaxed) + 1;
            if (count <= 4 || (count % 1800) == 0)
                Log("[FOVCHAN] channels=%u ch0 %.4f -> %.4f rad count=%llu",
                    written, previous, radians,
                    static_cast<unsigned long long>(count));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

#include "camera_motion_trace.inl"
#include "render_motion_trace.inl"
#include "right_camera_sync.h"
#include "fov_transition_fix.h"

void BeginSepProbeCameraJob(void* camera)
{
    ApplyGameFovScale(camera);
    ProbeFovSink(camera);
    DriveFovSink(camera);
    g_sepProbeActiveCamera = camera;
    g_sepProbeActiveJobSequence =
        g_sepProbeJobSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    g_nativeEyeBuild = {};
    g_nativeEyeBuild.jobSequence = g_sepProbeActiveJobSequence;
    BeginCameraMotionTrace(camera);
    RecordSepProbeEvent(0, nullptr);
    const auto armed = g_camDumpGeneration.load(std::memory_order_acquire);
    if (camera && armed &&
        g_camObjDumpedGeneration.load(std::memory_order_acquire) < armed &&
        g_stereoEnabled.load(std::memory_order_relaxed))
    {
        auto expected = g_camObjDumpedGeneration.load(std::memory_order_acquire);
        if (expected < armed && g_camObjDumpedGeneration.compare_exchange_strong(
                expected, armed, std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            Log("[CAMOBJ_HDR] gen=%llu camera=%p job=%llu present=%llu "
                "output=%d",
                static_cast<unsigned long long>(armed), camera,
                static_cast<unsigned long long>(g_sepProbeActiveJobSequence),
                static_cast<unsigned long long>(
                    g_presentCount.load(std::memory_order_relaxed)),
                static_cast<int>(ReadTrueOutput()));
            DumpProbeBytes("CAMOBJ", armed, camera, 0x800);
        }
    }
}

void EndSepProbeCameraJob()
{
    EndCameraMotionTrace();
    RecordSepProbeEvent(3, nullptr);
    g_sepProbeActiveCamera = nullptr;
    g_sepProbeActiveJobSequence = 0;
}

void CaptureSepProbeMatrix(std::size_t kind, const float* matrix)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (kind >= g_sepProbeMatrices.size() || !matrix ||
        !g_stereoEnabled.load(std::memory_order_acquire) ||
        !IsReadable(matrix, sizeof(float) * 16))
        return;
    const auto eye = static_cast<std::size_t>(ReadTrueOutput() & 1);
    auto& slot = g_sepProbeMatrices[kind][eye];
    if (!TryAcquireSRWLockExclusive(&slot.lock))
        return;
    bool copied = false;
    __try
    {
        std::memcpy(slot.matrix, matrix, sizeof(slot.matrix));
        copied = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        copied = false;
    }
    if (copied)
    {
        ++slot.writes;
        slot.threadId = GetCurrentThreadId();
        slot.separation = g_stereoSeparationAddress ? *g_stereoSeparationAddress : -1.0f;
        slot.baseline = g_stereoBaselineAddress ? *g_stereoBaselineAddress : -1.0f;
    }
    ReleaseSRWLockExclusive(&slot.lock);
    RecordSepProbeEvent(kind == 0 ? 1u : 2u, matrix);
}

float* BuildNativeEyePair(std::size_t kind, EyeMatrixBuilderFn builder,
    void* stereo, float* output, const float* source, float scalar,
    std::uint32_t metadata)
{
    const auto armedGeneration =
        g_camDumpGeneration.load(std::memory_order_acquire);
    if (armedGeneration && kind <= 1 &&
        g_stereoEnabled.load(std::memory_order_relaxed))
    {
        auto expected =
            g_camSrcDumpedGeneration[kind].load(std::memory_order_acquire);
        if (expected < armedGeneration &&
            g_camSrcDumpedGeneration[kind].compare_exchange_strong(expected,
                armedGeneration, std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            Log("[CAMSRC_HDR] gen=%llu kind=%s source=%p stereo=%p "
                "scalar=%.6f metadata=%u job=%llu output=%d",
                static_cast<unsigned long long>(armedGeneration),
                kind == 0 ? "proj" : "view", static_cast<const void*>(source),
                stereo, scalar, metadata,
                static_cast<unsigned long long>(g_sepProbeActiveJobSequence),
                static_cast<int>(ReadTrueOutput()));
            DumpProbeBytes(kind == 0 ? "CAMSRC_P" : "CAMSRC_V",
                armedGeneration, source, 0x140);
        }
        auto stereoExpected =
            g_camStereoDumpedGeneration.load(std::memory_order_acquire);
        if (stereoExpected < armedGeneration &&
            g_camStereoDumpedGeneration.compare_exchange_strong(stereoExpected,
                armedGeneration, std::memory_order_acq_rel,
                std::memory_order_acquire))
            DumpProbeBytes("CAMSTEREO", armedGeneration, stereo, 0x100);
    }
    if (!builder || !output || kind > 1 ||
        !g_stereoEnabled.load(std::memory_order_acquire) ||
        ReadTrueOutput() != 1 || !g_sepProbeActiveJobSequence)
        return builder ? builder(stereo, output, source, scalar, metadata) : output;

    int* outputCell = ResolveCurrentOutputCell();
    if (!outputCell)
        return builder(stereo, output, source, scalar, metadata);
    const int savedOutput = *outputCell;
    if (savedOutput != 0 && savedOutput != 1)
        return builder(stereo, output, source, scalar, metadata);

    alignas(16) float pair[2][16]{};
    for (int eye = 0; eye < 2; ++eye)
    {
        *outputCell = eye;
        builder(stereo, pair[eye], source, scalar, metadata);
    }
    *outputCell = savedOutput;

    std::memcpy(output, pair[savedOutput], sizeof(pair[0]));
    std::memcpy(g_nativeEyeBuild.matrices[kind], pair, sizeof(pair));
    g_nativeEyeBuild.jobSequence = g_sepProbeActiveJobSequence;
    g_nativeEyeBuild.ready[kind] = true;
    g_nativeEyePairsBuilt.fetch_add(1, std::memory_order_relaxed);
    if (TryAcquireSRWLockExclusive(&g_nativeEyePairLatest.lock))
    {
        auto& retained = g_nativeEyePairLatest;
        if (retained.jobSequence != g_sepProbeActiveJobSequence)
            retained.readyMask = 0;
        std::memcpy(retained.matrices[kind], pair, sizeof(pair));
        retained.jobSequence = g_sepProbeActiveJobSequence;
        retained.present = g_presentCount.load(std::memory_order_relaxed);
        retained.threadId = GetCurrentThreadId();
        retained.readyMask |= 1u << kind;
        retained.separation =
            g_stereoSeparationAddress ? *g_stereoSeparationAddress : -1.0f;
        retained.baseline =
            g_stereoBaselineAddress ? *g_stereoBaselineAddress : -1.0f;
        ReleaseSRWLockExclusive(&retained.lock);
    }
    return output;
}

// Right-eye translation at the camera source.  The SWAYCAM run
// proved two things: the camera job builds ONLY the right pass (only the
// right half rocked), and this seat steers that pass's full geometry.  The
// left half renders from the mono gameplay camera by engine design - there
// is no left camera build to modify.  So the whole separation goes to the
// right eye: translate the right pass's camera by one full eye distance
// (the engine baseline, world units) along camera-right, transiently around
// each real builder call.  Left eye = original camera; this is a valid
// asymmetric stereo rig with REAL parallax.  F10 scales baseline and
// therefore the eye distance.  The proj builder's `source` starts at the
// camera world transform (basis rows 0..2, world position row 3, w=1); the
// view builder's `source` is the same struct advanced 0x40.  Validation
// (w==1, unit right row) keeps a wrong layout from being touched.
struct CamSourceSwayState
{
    bool applied{};
    float* positionRow{};
    float saved[4]{};
};
thread_local CamSourceSwayState g_camSourceSway[2]{};
std::atomic_uint64_t g_camSwayApplied{};
std::atomic_bool g_camSwayLogged{};

// [LENSPIN] The engine's lens GETTER, found in Ghidra:
//   +0x9193380  float getter(mgr) = [[mgr+0x2FD8]+0x14] + [[mgr+0x2E18]+0x14]
//               (additive lens channel + winning lens channel), 12 callers
//               incl. the projection builders at +0x92D92CB / +0x92D92FA;
//   +0x91934A0  the same sum inline, then the vcall [[mgr+0x2AE0]]+0x78 and a
//               tail jump into the lens convert +0x92D5D50(fov, v).
// Pinning the channel table of the ONE manager captured at the FOV setter
// misses a camera that never calls the setter (intro: stock 48 deg for
// 30 s), a new camera manager and the additive zoom term (
// 30 deg), each showing as a small picture with black around it.  Both
// functions are replaced by jumps into these, so every camera's final lens is
// the pinned one while VR is on, and the untouched sum otherwise.
constexpr std::uintptr_t kLensGetterRva = 0x9193380;
constexpr std::uintptr_t kLensSiblingRva = 0x91934A0;
constexpr std::uintptr_t kLensConvertRva = 0x92D5D50;
constexpr std::array<std::uint8_t, 25> kLensGetterPrefix{
    0x48, 0x8B, 0x81, 0xD8, 0x2F, 0x00, 0x00, 0x48, 0x8B, 0x89, 0x18, 0x2E, 0x00, 0x00,
    0xF3, 0x0F, 0x10, 0x40, 0x14, 0xF3, 0x0F, 0x58, 0x41, 0x14, 0xC3};
constexpr std::array<std::uint8_t, 18> kLensSiblingPrefix{
    0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0x81, 0xD8, 0x2F, 0x00, 0x00, 0x48, 0x8B, 0x91,
    0x18, 0x2E, 0x00, 0x00};
CodePatch g_lensGetterPatch{};
CodePatch g_lensSiblingPatch{};
std::atomic_bool g_lensPinEnabled{true};
std::atomic_uint64_t g_lensCalls{}, g_lensChanged{};

float LensPinned(const std::uint8_t* manager)
{
    const auto* extra = *reinterpret_cast<const std::uint8_t* const*>(manager + 0x2FD8);
    const auto* winner = *reinterpret_cast<const std::uint8_t* const*>(manager + 0x2E18);
    const float extraLens = *reinterpret_cast<const float*>(extra + 0x14);
    const float winnerLens = *reinterpret_cast<const float*>(winner + 0x14);
    const float original = extraLens + winnerLens;
    g_lensCalls.fetch_add(1, std::memory_order_relaxed);
    if (!g_lensPinEnabled.load(std::memory_order_relaxed) ||
        !(VrCameraEnabled() || g_camParamAllModes.load(std::memory_order_relaxed)))
        return original;
    const float degrees = g_liveFovDegrees.load(std::memory_order_relaxed);
    if (!(degrees > 10.0f && degrees < 179.0f) || !(original > 0.05f && original < 3.1f))
        return original;
    constexpr float kPi = 3.14159265358979323846f;
    const float pinned = (degrees > 87.0f ? 87.0f : degrees) * kPi / 180.0f;
    if (std::fabs(original - pinned) > 0.0175f)
    {
        const auto changed = g_lensChanged.fetch_add(1, std::memory_order_relaxed) + 1;
        if (changed <= 12 || (changed % 3600) == 0)
            Log("[LENSPIN] mgr=%p lens %.1f deg (winner %.1f + extra %.1f) -> pinned %.1f deg (changed=%llu calls=%llu)",
                static_cast<const void*>(manager), original * 180.0f / kPi,
                winnerLens * 180.0f / kPi, extraLens * 180.0f / kPi, pinned * 180.0f / kPi,
                static_cast<unsigned long long>(changed),
                static_cast<unsigned long long>(g_lensCalls.load(std::memory_order_relaxed)));
    }
    return pinned;
}
float __fastcall LensGetterDetour(const std::uint8_t* manager)
{
    return LensPinned(manager);
}
float __fastcall LensSiblingDetour(std::uint8_t* manager)
{
    const float lens = LensPinned(manager);
    void* object = *reinterpret_cast<void**>(manager + 0x2AE0);
    using VirtualFn = float(__fastcall*)(void*, int);
    const VirtualFn second = (*reinterpret_cast<VirtualFn**>(object))[0x78 / sizeof(void*)];
    const float value = second(object, 0);
    using ConvertFn = float(__fastcall*)(float, float);
    return reinterpret_cast<ConvertFn>(g_exeBase + kLensConvertRva)(lens, value);
}

// [LENSWATCH] The rendered lens leaving the pin (Ifrit prologue:
// 87 deg -> ~41 deg and closing, black around a small picture,
// because the headset claims exactly the lens the game rendered).  Logs every
// off-pin episode: start, progress every 90 presents, end with its range, plus
// what the FOV setter / scripted camera were doing.  Numpad 9 adds a snapshot.
std::atomic_bool g_lensSnapshot{false};
void LensContext(const char* what, std::uint64_t present, float vfov, float target, float focalY)
{
    const auto lastHit = g_fovLastHitPresent.load(std::memory_order_relaxed);
    Log("[LENSWATCH] %s present=%llu rendered vfov=%.1f deg (pinned %.1f) P11=%.4f | FOV setter: channel=%u last hit %lld presents ago mgr=%llX hits=%llu | stereo=%d aer=%d conversation=%d",
        what, static_cast<unsigned long long>(present), vfov, target, focalY,
        g_fovLastChannel.load(std::memory_order_relaxed),
        lastHit ? static_cast<long long>(present - lastHit) : -1LL,
        static_cast<unsigned long long>(g_fovSetManager.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_fovSetHits.load(std::memory_order_relaxed)),
        g_stereoEnabled.load(std::memory_order_relaxed) ? 1 : 0,
        g_aerEnabled.load(std::memory_order_relaxed) ? 1 : 0,
        g_conversationActive.load(std::memory_order_relaxed));
}
void LensWatch(float focalY)
{
    constexpr float kPi = 3.14159265358979323846f;
    if (!(focalY > 0.01f))
        return;
    const float vfov = 2.0f * std::atan(1.0f / focalY) * 180.0f / kPi;
    const float pinned = g_liveFovDegrees.load(std::memory_order_relaxed);
    const float target = pinned > 87.0f ? 87.0f : pinned;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    static bool active = false;
    static std::uint64_t since = 0, frames = 0, lastProgress = 0, episodes = 0;
    static float lo = 0.0f, hi = 0.0f;
    const bool off = std::fabs(vfov - target) > 2.0f;
    if (off && !active)
    {
        active = true;
        since = present;
        frames = 0;
        lo = hi = vfov;
        lastProgress = present;
        if (++episodes <= 300)
            LensContext("OFF-PIN START", present, vfov, target, focalY);
    }
    if (active)
    {
        ++frames;
        lo = vfov < lo ? vfov : lo;
        hi = vfov > hi ? vfov : hi;
        if (off && present - lastProgress >= 90 && episodes <= 300)
        {
            lastProgress = present;
            LensContext("off-pin continuing", present, vfov, target, focalY);
        }
        if (!off)
        {
            if (episodes <= 300)
                Log("[LENSWATCH] back on pin after %llu presents (%llu builds), rendered vfov range %.1f..%.1f deg",
                    static_cast<unsigned long long>(present - since),
                    static_cast<unsigned long long>(frames), lo, hi);
            active = false;
        }
    }
    if (g_lensSnapshot.exchange(false, std::memory_order_acq_rel))
        LensContext("USER MARK snapshot", present, vfov, target, focalY);
}

void ApplyCamSourceSway(std::size_t kind, const float* source)
{
    auto& state = g_camSourceSway[kind & 1];
    state.applied = false;
    if (!source || kind > 1 ||
        g_o0SkewProbeMode.load(std::memory_order_relaxed) !=
            kO0SkewModeSwayCam ||
        !g_stereoEnabled.load(std::memory_order_relaxed))
        return;
    float* transform =
        const_cast<float*>(kind == 0 ? source : source - 16);
    if (!IsReadable(transform, 64))
        return;
    __try
    {
        const float w = transform[15];
        // [VIEWSEAT] This transform is a WORLD-TO-VIEW matrix (row-vector):
        // camera axes in the columns, and row 3 = -(eye . axis) - the eye
        // position expressed in the camera's own axes, NOT a world position.
        // Proof: row 3 keeps |row3| ~ 2500 m at every camera
        // heading while its components flip sign as the camera circles
        // Noctis ((1972,-242,1494) at 130 deg, (248,70,-2477) at -108 deg);
        // a world position moving metres cannot do that, a view translation
        // must.  Moving the eye right by d therefore changes exactly one
        // number: row3.x -= d.  Adding a WORLD vector to row 3 (row 0 =
        // world X; column 0 = another mix) swings with camera heading and
        // doubles the image.
        const float rightX = transform[0];
        const float rightY = transform[4];
        const float rightZ = transform[8];
        const float lengthSquared =
            rightX * rightX + rightY * rightY + rightZ * rightZ;
        if (!(w > 0.999f && w < 1.001f) ||
            !(lengthSquared > 0.9f && lengthSquared < 1.1f))
            return;
        if (kind == 0)
        {
            // Publish the live projection (P at source + 0x40) so the OpenXR
            // presenter can claim the frustum the game actually rendered.
            const float focalX = transform[16];
            const float focalY = transform[21];
            const float projectionSkew = transform[24];
            if (focalX > 0.3f && focalX < 5.0f && focalY > 0.5f &&
                focalY < 8.0f && projectionSkew > -1.0f && projectionSkew < 1.0f)
            {
                RetailXr::SetGameProjection(focalX, focalY, projectionSkew,
                    focalX, focalY);
                LensWatch(focalY);   // [LENSWATCH]
                static std::atomic<float> lastLoggedFocalX{0.0f};
                static std::atomic_uint32_t focalLogCount{0};
                const float previous =
                    lastLoggedFocalX.load(std::memory_order_relaxed);
                const float delta = focalX - previous;
                if ((delta > 0.01f || delta < -0.01f) &&
                    focalLogCount.fetch_add(1, std::memory_order_relaxed) < 10)
                {
                    lastLoggedFocalX.store(focalX, std::memory_order_relaxed);
                    Log("[FOVLIVE] P00=%.4f P11=%.4f skew=%.4f", focalX,
                        focalY, projectionSkew);
                }
            }
        }
        // Belt and braces: the job only ever builds output 1, but if a left
        // build ever appears it must stay mono.
        if (ReadTrueOutput() != 1)
            return;
        if (kind == 0)
            CaptureValidRightCameraSource(transform);
        CaptureRenderMotion(2u + static_cast<unsigned>(kind), transform);
        // Engine baseline is centimeters; the world is meters.  The raw
        // value (6.0) produced a six-meter eye separation.
        float offset = 0.0f;
        if (g_stereoBaselineAddress)
            offset = *g_stereoBaselineAddress * 0.01f;
        if (!(offset == offset) || offset < -5.0f || offset > 5.0f ||
            (offset > -1e-4f && offset < 1e-4f))
            return;
        float* position = transform + 12;
        state.positionRow = position;
        for (int index = 0; index < 4; ++index)
            state.saved[index] = position[index];
        // [MESWAP] Swap eyes flips the camera sign together with the eye
        // routing: depth stays matched, the convergence
        // plane inverts.  Right-eye seat: +b normally, -b swapped.
        position[0] -= RetailXr::GetSplitSwapEyes() ? -offset : offset;
        state.applied = true;
        if (kind == 0)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                g_splitRightSource[axis].store(state.saved[axis],
                    std::memory_order_relaxed);
                g_splitRightFinal[axis].store(position[axis],
                    std::memory_order_relaxed);
            }
            g_splitRightSamples.fetch_add(1, std::memory_order_relaxed);
        }
        g_camSwayApplied.fetch_add(1, std::memory_order_relaxed);
        bool logExpected = false;
        if (g_camSwayLogged.compare_exchange_strong(logExpected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
            Log("[CAMSWAY] first application kind=%s transform=%p "
                "pos=(%.2f,%.2f,%.2f) right=(%.4f,%.4f,%.4f) offset=%.2f",
                kind == 0 ? "proj" : "view",
                static_cast<void*>(transform), state.saved[0],
                state.saved[1], state.saved[2], rightX, rightY, rightZ,
                offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        state.applied = false;
    }
}

void RestoreCamSourceSway(std::size_t kind)
{
    auto& state = g_camSourceSway[kind & 1];
    if (!state.applied || !state.positionRow)
        return;
    __try
    {
        for (int index = 0; index < 4; ++index)
            state.positionRow[index] = state.saved[index];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    state.applied = false;
    state.positionRow = nullptr;
}

extern "C" float* __fastcall RetailEyeViewBuilder(void* stereo, float* output,
    const float* source, float scalar, std::uint32_t metadata)
{
    CaptureCameraMotionBuilder(1, source);
    ApplyCamSourceSway(1, source);
    float* result = BuildNativeEyePair(1, g_realEyeViewBuilder, stereo, output,
        source, scalar, metadata);
    if (g_camSourceSway[1].applied)
        CaptureRenderMotion(5, result);
    RestoreCamSourceSway(1);
    return result;
}

// The moving trace matched bank0 to the preceding camera update in all
// 1062 adjacent samples. Select that already-established left render camera
// for the native right build, then apply the existing full baseline locally.
std::atomic_bool g_syncRightCameraToLeftRender{true};
std::atomic_uint64_t g_rightCameraSyncApplied{};
std::atomic_uint64_t g_rightCameraSyncRejected{};
bool PrepareMatchingRightSource(const float* source, float* local)
{
    if (!source || !g_syncRightCameraToLeftRender.load(std::memory_order_acquire) ||
        !g_stereoEnabled.load(std::memory_order_acquire) || ReadTrueOutput()!=1 ||
        g_sepProbeActiveJobSequence ||
        g_o0SkewProbeMode.load(std::memory_order_relaxed)!=kO0SkewModeSwayCam)
        return false;
    bool selected=false;
    __try
    {
        const auto head=g_dynamicHeadSource.load(std::memory_order_acquire);
        auto* area=*reinterpret_cast<std::uint8_t**>(g_exeBase+0x45DF038);
        const auto renderer=*reinterpret_cast<std::uintptr_t*>(g_exeBase+0x4F29430);
        const auto bank=area ? *reinterpret_cast<std::uintptr_t*>(area+0x300) : 0;
        if(head && bank && renderer)
        {
            alignas(16) float original[32], headView[16], bankView[16], renderView[16], checkView[16];
            std::memcpy(original,source,sizeof(original));
            std::memcpy(headView,reinterpret_cast<const void*>(head),64);
            std::memcpy(bankView,reinterpret_cast<const void*>(bank),64);
            std::memcpy(renderView,reinterpret_cast<const void*>(renderer+0xB804C0),64);
            std::memcpy(checkView,reinterpret_cast<const void*>(bank),64);
            if (std::memcmp(bankView,checkView,64)==0)
                selected=SelectMatchingLeftRenderCamera(original,headView,bankView,renderView,local);
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    if(selected)
    {
        const auto count=g_rightCameraSyncApplied.fetch_add(1,std::memory_order_relaxed)+1;
        if(count<=3) Log("[RIGHTSYNC] native right camera uses matching left render view; local source; applied=%llu",static_cast<unsigned long long>(count));
    }
    else g_rightCameraSyncRejected.fetch_add(1,std::memory_order_relaxed);
    return selected;
}

extern "C" float* __fastcall RetailEyeProjectionBuilder(void* stereo,
    float* output, const float* source, float scalar, std::uint32_t metadata)
{
    CaptureCameraMotionBuilder(0, source);
    alignas(16) float localSource[32]{};
    if (PrepareMatchingRightSource(source, localSource))
        source = localSource;
    ApplyCamSourceSway(0, source);
    float* result = BuildNativeEyePair(0, g_realEyeProjectionBuilder, stereo,
        output, source, scalar, metadata);
    if (g_camSourceSway[0].applied)
        CaptureRenderMotion(4, result);
    RestoreCamSourceSway(0);
    return result;
}

bool CommitSymmetricEyePair(std::size_t kind, SepProbeUploadFn upload, void* manager)
{
    if (!upload || !manager || !g_matrixInverse || kind > 1 ||
        !g_stereoEnabled.load(std::memory_order_acquire) || ReadTrueOutput() != 1 ||
        !g_sepProbeActiveJobSequence ||
        g_nativeEyeBuild.jobSequence != g_sepProbeActiveJobSequence ||
        !g_nativeEyeBuild.ready[kind])
        return false;

    alignas(16) float inverse0[16]{};
    alignas(16) float inverse1[16]{};
    g_matrixInverse(g_nativeEyeBuild.matrices[kind][0], inverse0);
    g_matrixInverse(g_nativeEyeBuild.matrices[kind][1], inverse1);
    upload(manager, g_nativeEyeBuild.matrices[kind][0], inverse0, 0);
    upload(manager, g_nativeEyeBuild.matrices[kind][1], inverse1, 1);
    g_nativeEyeBuild.ready[kind] = false;
    g_symmetricEyeCommits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void RouteSepProbeUpload(std::size_t kind, SepProbeUploadFn upload, void* manager,
    const float* matrix, const float* auxiliary, std::uint32_t argument)
{
    if (CommitSymmetricEyePair(kind, upload, manager))
        return;
    CaptureSepProbeMatrix(kind, matrix);
    if (upload)
        upload(manager, matrix, auxiliary, argument);
    if (g_stereoEnabled.load(std::memory_order_relaxed))
        g_symmetricEyeFallbacks.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void __fastcall RetailSepProbeProjectionUpload(void* manager,
    const float* matrix, const float* auxiliary, std::uint32_t argument)
{
    RouteSepProbeUpload(0, g_realSepProbeProjectionUpload, manager, matrix,
        auxiliary, argument);
}

extern "C" void __fastcall RetailSepProbeViewUpload(void* manager,
    const float* matrix, const float* auxiliary, std::uint32_t argument)
{
    RouteSepProbeUpload(1, g_realSepProbeViewUpload, manager, matrix,
        auxiliary, argument);
}

using PassQueueFn = void(__fastcall*)(void*, void*, const char*, std::uint64_t, std::uint64_t);
using UploadMatricesFn = void(__fastcall*)(void*);
PassQueueFn g_realPassQueue{};
UploadMatricesFn g_realUploadMatrices{};

bool ReadPassName(const char* source, char (&name)[48])
{
    if (!source)
        return false;
    __try
    {
        std::size_t index{};
        for (; index + 1 < _countof(name); ++index)
        {
            const char value = source[index];
            if (value == '\0')
                break;
            if (value < 0x20 || value > 0x7E)
                return false;
            name[index] = value;
        }
        name[index] = '\0';
        return index != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        name[0] = '\0';
        return false;
    }
}

void RecordPassName(const char* name)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!name || !*name)
        return;
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto* cursor = reinterpret_cast<const unsigned char*>(name); *cursor; ++cursor)
        hash = (hash ^ *cursor) * 1099511628211ull;
    if (hash == 0)
        hash = 1;
    const auto start = static_cast<std::size_t>((hash ^ (hash >> 32)) &
        (kPassNameSampleCount - 1));
    for (std::size_t probe = 0; probe < 32; ++probe)
    {
        auto& slot = g_passNames[(start + probe) & (kPassNameSampleCount - 1)];
        auto existing = slot.key.load(std::memory_order_acquire);
        if (existing == 0)
        {
            if (!slot.key.compare_exchange_strong(existing, hash,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            std::memcpy(slot.name, name, sizeof(slot.name));
            slot.name[sizeof(slot.name) - 1] = '\0';
            slot.ready.store(1, std::memory_order_release);
        }
        else if (existing != hash)
            continue;
        while (!slot.ready.load(std::memory_order_acquire))
            YieldProcessor();
        const auto bucket = g_stereoEnabled.load(std::memory_order_relaxed)
            ? static_cast<std::size_t>(1 + (ReadTrueOutput() & 1)) : 0;
        slot.calls[bucket].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_passNamesDropped.fetch_add(1, std::memory_order_relaxed);
}

void LogPassNameTelemetry()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    for (auto& slot : g_passNames)
    {
        if (!slot.ready.load(std::memory_order_acquire))
            continue;
        std::uint64_t calls[3]{};
        for (std::size_t bucket = 0; bucket < 3; ++bucket)
            calls[bucket] = slot.calls[bucket].exchange(0, std::memory_order_acq_rel);
        if (calls[0] || calls[1] || calls[2])
            Log("[PASSNAME] name='%s' calls(mono/o0/o1)=%llu/%llu/%llu",
                slot.name, static_cast<unsigned long long>(calls[0]),
                static_cast<unsigned long long>(calls[1]),
                static_cast<unsigned long long>(calls[2]));
    }
    const auto dropped = g_passNamesDropped.exchange(0, std::memory_order_acq_rel);
    if (dropped)
        Log("[PASSNAME] dropped=%llu", static_cast<unsigned long long>(dropped));
}

// Read-only association: bank0 is the left camera already paired with the
// right eye. Do not modify any engine matrix or rendering decisions here.
// [MONOCAM] Read-only measurement.  In Mono/AER the rendered
// camera never equals a committed one (RENDERCAM match 0/4286) while in
// stereo it always does.  This compares the renderer's view at upload time
// with the last 8 committed cameras: exact (which one), a blend between two
// consecutive ones, or neither (and by how much).
struct CommitSlot { std::atomic_uint64_t seq{}; float m[16]{}; };
CommitSlot g_commitRing[8];
std::atomic_uint64_t g_commitSeq{};
void PushCommittedCamera(const float* m)
{
    const auto seq = g_commitSeq.load(std::memory_order_relaxed) + 1;
    auto& slot = g_commitRing[seq & 7];
    slot.seq.store(0, std::memory_order_release);
    std::memcpy(slot.m, m, sizeof(slot.m));
    slot.seq.store(seq, std::memory_order_release);
    g_commitSeq.store(seq, std::memory_order_release);
}
float ViewAngleDeg(const float* a, const float* b)
{
    float tr = 0.0f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            tr += a[r * 4 + c] * b[r * 4 + c];
    const float x = std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f);
    return std::acos(x) * 57.2957795f;
}
void ViewEye(const float* m, float* eye)
{
    for (int k = 0; k < 3; ++k)
        eye[k] = -(m[12] * m[k * 4 + 0] + m[13] * m[k * 4 + 1] + m[14] * m[k * 4 + 2]);
}
struct MonoCamStats
{
    std::uint64_t samples{}, invalid{}, exact[8]{}, blend{}, neither{};
    double blendT{}, missAngle{}, missAngleMax{}, stepAngle{}, missMetres{};
};
MonoCamStats g_monoCam{};
void MeasureRenderedCamera(const float* render)
{
    float c[8][16]{};
    int n = 0;
    const auto newest = g_commitSeq.load(std::memory_order_acquire);
    for (int k = 0; k < 8 && newest > static_cast<std::uint64_t>(k); ++k)
    {
        const auto want = newest - k;
        auto& slot = g_commitRing[want & 7];
        std::memcpy(c[n], slot.m, sizeof(c[n]));
        if (slot.seq.load(std::memory_order_acquire) != want) break;
        ++n;
    }
    auto& s = g_monoCam;
    ++s.samples;
    if (n < 2) { ++s.invalid; return; }
    float re[3]{};
    ViewEye(render, re);
    int best = -1; float bestAngle = 1e9f, bestMetres = 0.0f;
    for (int k = 0; k < n; ++k)
    {
        float ce[3]{};
        ViewEye(c[k], ce);
        const float a = ViewAngleDeg(render, c[k]);
        const float d = std::sqrt((re[0] - ce[0]) * (re[0] - ce[0]) + (re[1] - ce[1]) * (re[1] - ce[1]) + (re[2] - ce[2]) * (re[2] - ce[2]));
        if (a < bestAngle) { bestAngle = a; best = k; bestMetres = d; }
    }
    s.stepAngle += ViewAngleDeg(c[0], c[1]);
    if (bestAngle < 0.002f && bestMetres < 0.001f) { ++s.exact[best]; return; }
    // between two consecutive committed cameras?
    for (int k = 0; k + 1 < n; ++k)
    {
        const float ab = ViewAngleDeg(c[k], c[k + 1]);
        const float ar = ViewAngleDeg(render, c[k + 1]);
        const float rb = ViewAngleDeg(render, c[k]);
        if (ab > 0.01f && ar + rb - ab < 0.02f + 0.05f * ab)
        {
            ++s.blend; s.blendT += ar / ab;   // 0 = older camera, 1 = newer
            return;
        }
    }
    ++s.neither;
    s.missAngle += bestAngle; s.missMetres += bestMetres;
    if (bestAngle > s.missAngleMax) s.missAngleMax = bestAngle;
}
void ReportRenderedCamera()
{
    auto& s = g_monoCam;
    if (s.samples < 600) return;
    const double miss = s.neither ? static_cast<double>(s.neither) : 1.0;
    Log("[MONOCAM] rendered vs committed camera over %llu frames: exact newest/1/2/3+ back=%llu/%llu/%llu/%llu | "
        "blend of two=%llu (avg %.2f toward newer) | neither=%llu (avg %.3f deg %.4f m off, max %.3f deg) | "
        "camera turns %.3f deg/frame | no commits=%llu",
        static_cast<unsigned long long>(s.samples),
        static_cast<unsigned long long>(s.exact[0]), static_cast<unsigned long long>(s.exact[1]),
        static_cast<unsigned long long>(s.exact[2]),
        static_cast<unsigned long long>(s.exact[3] + s.exact[4] + s.exact[5] + s.exact[6] + s.exact[7]),
        static_cast<unsigned long long>(s.blend), s.blend ? s.blendT / s.blend : 0.0,
        static_cast<unsigned long long>(s.neither), s.missAngle / miss, s.missMetres / miss, s.missAngleMax,
        s.stepAngle / static_cast<double>(s.samples), static_cast<unsigned long long>(s.invalid));
    s = {};
}

void CaptureNativeRenderHeadPose()
{
    // [RENDERCAM] stereo, AER and Mono: one upload per rendered frame.
    if (!VrCameraEnabled()) return;
    if (g_aerEnabled.load(std::memory_order_acquire))
    {
        alignas(16) float render[16]{};
        bool ok = false;
        __try
        {
            const auto renderer = *reinterpret_cast<std::uintptr_t*>(g_exeBase + 0x4F29430);
            if (renderer)
            {
                std::memcpy(render, reinterpret_cast<const void*>(renderer + 0xB804C0), 64);
                ok = IsSyncCameraView(render);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (ok) MeasureRenderedCamera(render); else ++g_monoCam.invalid;
        ReportRenderedCamera();
        // [MONOTAG] single-output render: this view is the camera drawn this
        // frame (the stereo bank below is not written in Mono/AER).
        RetailXr::RecordNativeRenderCamera(ok ? render : nullptr, true);
        return;
    }
    alignas(16) float bankView[16]{}, check[16]{}, renderView[16]{};
    bool valid = false;
    __try
    {
        auto* area = *reinterpret_cast<std::uint8_t**>(g_exeBase + 0x45DF038);
        const auto renderer = *reinterpret_cast<std::uintptr_t*>(g_exeBase + 0x4F29430);
        const auto bank = area ? *reinterpret_cast<std::uintptr_t*>(area + 0x300) : 0;
        if (bank && renderer)
        {
            std::memcpy(bankView, reinterpret_cast<const void*>(bank), 64);
            std::memcpy(renderView, reinterpret_cast<const void*>(renderer + 0xB804C0), 64);
            std::memcpy(check, reinterpret_cast<const void*>(bank), 64);
            valid = std::memcmp(bankView, check, 64) == 0 &&
                IsSyncCameraView(bankView) && IsSyncCameraView(renderView);
            // Renderer can currently describe either eye; translation differs
            // by the baseline, but its orientation must agree with bank0.
            for (int cell = 0; cell < 12 && valid; ++cell)
                valid = bankView[cell] == renderView[cell];
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { valid = false; }
    RetailXr::RecordNativeRenderCamera(valid ? bankView : nullptr);
}

extern "C" void __fastcall RetailUploadMatricesPolicy(void* manager)
{
    const int output = ReadTrueOutput() & 1;
    (output == 0 ? g_uploadMatricesOutput0 : g_uploadMatricesOutput1)
        .fetch_add(1, std::memory_order_relaxed);
    if (output == 0 && manager && g_stereoEnabled.load(std::memory_order_acquire))
    {
        g_uploadMatricesManager.store(reinterpret_cast<std::uintptr_t>(manager),
            std::memory_order_release);
        g_uploadMatricesArmed.store(1, std::memory_order_release);
    }
    CaptureNativeRenderHeadPose();
    CaptureRenderMotion(7, nullptr);
    if (g_realUploadMatrices)
        g_realUploadMatrices(manager);
    CaptureRenderMotion(8, nullptr);
}

extern "C" void __fastcall RetailPassQueuePolicy(void* queue, void* context,
    const char* passName, std::uint64_t flags, std::uint64_t size)
{
    char name[48]{};
    const bool named = (ResearchDiagnostics::Enabled || kInjectUploadMatrices) && ReadPassName(passName, name);
    if (named)
        RecordPassName(name);
    if constexpr (kInjectUploadMatrices)
    {
        const int output = ReadTrueOutput() & 1;
        if (named && output == 1 && g_stereoEnabled.load(std::memory_order_acquire) &&
            std::strcmp(name, "uploadMatrices") != 0 &&
            g_uploadMatricesArmed.exchange(0, std::memory_order_acq_rel) != 0)
        {
            const auto manager = g_uploadMatricesManager.load(std::memory_order_acquire);
            if (manager && g_realUploadMatrices)
            {
                bool injected = false;
                __try
                {
                    g_realUploadMatrices(reinterpret_cast<void*>(manager));
                    injected = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    injected = false;
                }
                if (injected)
                {
                    const auto count = g_uploadMatricesInjected.fetch_add(1,
                        std::memory_order_relaxed) + 1;
                    if (count <= 8 || (count % 600) == 0)
                        Log("[UPMTX] injected output-1 uploadMatrices #%llu before pass '%s'",
                            static_cast<unsigned long long>(count), name);
                }
                else
                {
                    const auto failures = g_uploadMatricesInjectionFailed.fetch_add(1,
                        std::memory_order_relaxed) + 1;
                    Log("[UPMTX] injection failed #%llu; disabling captured manager",
                        static_cast<unsigned long long>(failures));
                    g_uploadMatricesManager.store(0, std::memory_order_release);
                }
            }
        }
    }
    if (g_realPassQueue)
        g_realPassQueue(queue, context, passName, flags, size);
}

std::uint64_t HashReadableBytes(const std::uint8_t* data, std::size_t size)
{
    if (!data || !IsReadable(data, size))
        return 0;
    std::uint64_t hash = 1469598103934665603ull;
    __try
    {
        for (std::size_t index = 0; index < size; ++index)
        {
            hash ^= data[index];
            hash *= 1099511628211ull;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    return hash;
}

void MaybeLogOutputBanks(std::uintptr_t callerRva, int output)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!g_stereoEnabled.load(std::memory_order_acquire) || callerRva != 0x0E8D51BF)
        return;
    const auto now = GetTickCount64();
    auto previous = g_lastBankTelemetryMs.load(std::memory_order_acquire);
    if (previous != 0 && now - previous < 1000)
        return;
    if (!g_lastBankTelemetryMs.compare_exchange_strong(previous, now,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    std::uint8_t* area{};
    std::uint8_t* bank0{};
    std::uint8_t* bank1{};
    std::uint32_t handles0{};
    std::uint32_t handles1{};
    __try
    {
        area = *reinterpret_cast<std::uint8_t**>(g_exeBase + kDisplayAreaObjectGlobalRva);
        if (area && IsReadable(area + 0x300, 16))
        {
            bank0 = *reinterpret_cast<std::uint8_t**>(area + 0x300);
            bank1 = *reinterpret_cast<std::uint8_t**>(area + 0x308);
        }
        for (std::size_t offset = 0x280; offset <= 0x2A0; offset += 8)
        {
            if (bank0 && IsReadable(bank0 + offset, sizeof(std::uintptr_t)) &&
                *reinterpret_cast<const std::uintptr_t*>(bank0 + offset))
                ++handles0;
            if (bank1 && IsReadable(bank1 + offset, sizeof(std::uintptr_t)) &&
                *reinterpret_cast<const std::uintptr_t*>(bank1 + offset))
                ++handles1;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        area = nullptr;
        bank0 = nullptr;
        bank1 = nullptr;
        handles0 = 0;
        handles1 = 0;
    }

    Log("[BANK] caller=0x%llX output=%d area=%p bank0=%p bank1=%p distinct=%d "
        "handles=%u/%u cameraHash=%016llX/%016llX stateHash=%016llX/%016llX",
        static_cast<unsigned long long>(callerRva), output, area, bank0, bank1,
        bank0 && bank1 && bank0 != bank1 ? 1 : 0, handles0, handles1,
        static_cast<unsigned long long>(HashReadableBytes(bank0, 0x280)),
        static_cast<unsigned long long>(HashReadableBytes(bank1, 0x280)),
        static_cast<unsigned long long>(HashReadableBytes(bank0 ? bank0 + 0x2A8 : nullptr, 8)),
        static_cast<unsigned long long>(HashReadableBytes(bank1 ? bank1 + 0x2A8 : nullptr, 8)));
}

void RecordOutputCaller(std::uintptr_t rva, std::size_t bucket)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!rva || bucket >= 3)
        return;
    const auto start = static_cast<std::size_t>((rva ^ (rva >> 13)) &
        (kOutputCallerSampleCount - 1));
    for (std::size_t probe = 0; probe < 24; ++probe)
    {
        auto& slot = g_outputCallers[(start + probe) & (kOutputCallerSampleCount - 1)];
        auto existing = slot.rva.load(std::memory_order_acquire);
        if (existing == 0 && !slot.rva.compare_exchange_strong(existing, rva,
                std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        if (existing != 0 && existing != rva)
            continue;
        slot.calls[bucket].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_outputCallersDropped.fetch_add(1, std::memory_order_relaxed);
}

void LogOutputCallerTelemetry()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    for (auto& slot : g_outputCallers)
    {
        const auto rva = slot.rva.load(std::memory_order_acquire);
        if (!rva)
            continue;
        std::uint64_t calls[3]{};
        for (std::size_t bucket = 0; bucket < 3; ++bucket)
            calls[bucket] = slot.calls[bucket].exchange(0, std::memory_order_acq_rel);
        if (calls[0] || calls[1] || calls[2])
            Log("[OUTCALL] caller=+0x%llX calls(mono/o0/o1)=%llu/%llu/%llu",
                static_cast<unsigned long long>(rva),
                static_cast<unsigned long long>(calls[0]),
                static_cast<unsigned long long>(calls[1]),
                static_cast<unsigned long long>(calls[2]));
    }
    const auto dropped = g_outputCallersDropped.exchange(0, std::memory_order_acq_rel);
    if (dropped)
        Log("[OUTCALL] dropped=%llu", static_cast<unsigned long long>(dropped));
}

void LogDispatchCallerTelemetry()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    for (auto& slot : g_dispatchCallers)
    {
        if (!slot.ready.load(std::memory_order_acquire))
            continue;
        std::uint64_t calls[3]{};
        for (std::size_t bucket = 0; bucket < 3; ++bucket)
            calls[bucket] = slot.calls[bucket].exchange(0, std::memory_order_acq_rel);
        if (calls[0] || calls[1] || calls[2])
            Log("[DISPATCH] caller=+0x%llX shader=%p groups=%u/%u/%u calls(mono/o0/o1)=%llu/%llu/%llu",
                static_cast<unsigned long long>(slot.rva), reinterpret_cast<void*>(slot.shader),
                slot.x, slot.y, slot.z,
                static_cast<unsigned long long>(calls[0]),
                static_cast<unsigned long long>(calls[1]),
                static_cast<unsigned long long>(calls[2]));
    }
    const auto dropped = g_dispatchCallersDropped.exchange(0, std::memory_order_acq_rel);
    if (dropped)
        Log("[DISPATCH] dropped=%llu", static_cast<unsigned long long>(dropped));
}

void LogProbeConsumerTelemetry()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    std::uint64_t draw4[3]{};
    for (std::size_t bucket = 0; bucket < 3; ++bucket)
        draw4[bucket] = g_draw4Calls[bucket].exchange(0, std::memory_order_acq_rel);
    if (draw4[0] || draw4[1] || draw4[2])
        Log("[DRAW4] calls(mono/o0/o1)=%llu/%llu/%llu",
            static_cast<unsigned long long>(draw4[0]),
            static_cast<unsigned long long>(draw4[1]),
            static_cast<unsigned long long>(draw4[2]));
    for (auto& sample : g_probeConsumers)
    {
        if (!sample.ready.load(std::memory_order_acquire))
            continue;
        std::uint64_t calls[3]{};
        std::uint64_t matches[3]{};
        for (std::size_t bucket = 0; bucket < 3; ++bucket)
        {
            calls[bucket] = sample.calls[bucket].exchange(0, std::memory_order_acq_rel);
            matches[bucket] = sample.currentMatches[bucket].exchange(
                0, std::memory_order_acq_rel);
        }
        if (calls[0] || calls[1] || calls[2])
            Log("[PROBECONSUME] shader=%p slot=%u res=%p calls(mono/o0/o1)=%llu/%llu/%llu currentMatch=%llu/%llu/%llu last=%p/%p/%p",
                reinterpret_cast<void*>(sample.shader), sample.slot,
                reinterpret_cast<void*>(sample.resource),
                static_cast<unsigned long long>(calls[0]),
                static_cast<unsigned long long>(calls[1]),
                static_cast<unsigned long long>(calls[2]),
                static_cast<unsigned long long>(matches[0]),
                static_cast<unsigned long long>(matches[1]),
                static_cast<unsigned long long>(matches[2]),
                reinterpret_cast<void*>(g_lastProbeWeightResource[0].load(
                    std::memory_order_acquire)),
                reinterpret_cast<void*>(g_lastProbeWeightResource[1].load(
                    std::memory_order_acquire)),
                reinterpret_cast<void*>(g_lastProbeWeightResource[2].load(
                    std::memory_order_acquire)));
    }
    const auto dropped = g_probeConsumersDropped.exchange(0, std::memory_order_acq_rel);
    if (dropped)
        Log("[PROBECONSUME] dropped=%llu", static_cast<unsigned long long>(dropped));
    const auto ao0 = g_monoAmbientAoProofApplied[0].exchange(0, std::memory_order_acq_rel);
    const auto ao1 = g_monoAmbientAoProofApplied[1].exchange(0, std::memory_order_acq_rel);
    const auto aoMissed = g_monoAmbientAoProofMissed.exchange(0, std::memory_order_acq_rel);
    if (ao0 || ao1 || aoMissed)
        Log("[AOPROOF] applied(o0/o1)=%llu/%llu missed=%llu learned=%llu",
            static_cast<unsigned long long>(ao0), static_cast<unsigned long long>(ao1),
            static_cast<unsigned long long>(aoMissed),
            static_cast<unsigned long long>(g_monoAmbientAoLearned.load(std::memory_order_relaxed)));
}

extern "C" int __fastcall RetailCurrentOutputPolicy(void*)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto rva = caller >= g_exeBase ? caller - g_exeBase : 0;
    const bool stereo = g_stereoEnabled.load(std::memory_order_relaxed);
    const int trueOutput = stereo ? ReadTrueOutput() : 0;
    RecordOutputCaller(rva, stereo ? static_cast<std::size_t>(1 + trueOutput) : 0);
    for (const auto lifecycleCaller : kLifecycleOutputCallers)
    {
        if (rva != lifecycleCaller)
            continue;
        const int output = trueOutput;
        MaybeLogOutputBanks(rva, output);
        if (output == 1)
            g_phaseThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        (output == 1 ? g_policyTrueLifecycle1 : g_policyTrueLifecycle0)
            .fetch_add(1, std::memory_order_relaxed);
        return output;
    }
    if (rva >= kFullCameraJobBeginRva && rva < kFullCameraJobEndRva)
    {
        g_policyTrueCamera.fetch_add(1, std::memory_order_relaxed);
        return trueOutput;
    }
    if (rva >= kPresenterBeginRva && rva < kPresenterEndRva)
    {
        g_policyTruePresenter.fetch_add(1, std::memory_order_relaxed);
        return trueOutput;
    }
    // Bank-rotator handoff gate (ret RVA of the getter call at +0x2BBBF1D).
    // The engine's own code reads `if (!multiOutput || output == 0)` before the
    // persistent final-image handoff bank[0x468] <- bank[0x490]; the broad
    // output-0 alias defeats that gate so the handoff re-runs on the eye-1
    // phase with a cleared/wrong source.  Returning the true output restores
    // the engine's intended multi-output behavior at exactly this decision.
    if (rva == kBankRotatorHandoffCallerRva &&
        g_bankRotGateTrue.load(std::memory_order_acquire))
    {
        g_policyTrueBankRotGate.fetch_add(1, std::memory_order_relaxed);
        return trueOutput;
    }
    if constexpr (kUseTrueOutputForDeferredManager)
    {
        if (rva == kDeferredManagerOutputCallerRva)
        {
            g_policyTrueDeferred.fetch_add(1, std::memory_order_relaxed);
            return trueOutput;
        }
    }
    if (rva == kDisplayAreaBankOutputCallerRva)
        CaptureRenderMotion(6, nullptr);
    if (g_useTrueOutputForDisplayAreaBank.load(std::memory_order_acquire))
    {
        if (rva == kDisplayAreaBankOutputCallerRva)
        {
            g_policyTrueDisplayAreaBank.fetch_add(1, std::memory_order_relaxed);
            return trueOutput;
        }
    }
    g_policyAliased.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

using MatrixCacheWriter = void(__fastcall*)(void*, std::uint32_t, const void*);
using MatrixCacheConstructor = void*(__fastcall*)(void*);
MatrixCacheWriter g_realMatrixCacheWriter{};
MatrixCacheConstructor g_matrixCacheConstructor{};

struct MatrixCacheView
{
    std::uintptr_t matrixArray{};
    std::uintptr_t flagsArray{};
    std::uint32_t count{};
    std::uint32_t current{};
};

bool ReadMatrixCacheView(const void* object, MatrixCacheView& view)
{
    __try
    {
        const auto* bytes = static_cast<const std::uint8_t*>(object);
        view.matrixArray = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x00);
        view.flagsArray = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x10);
        view.count = *reinterpret_cast<const std::uint32_t*>(bytes + 0x18);
        view.current = *reinterpret_cast<const std::uint32_t*>(bytes + 0x20);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        view = {};
        return false;
    }
}

bool IsValidMatrixCache(const MatrixCacheView& view, std::uint32_t index)
{
    return view.count > 0 && view.count <= 128 && index < view.count && view.current < view.count &&
        (view.matrixArray & 0xF) == 0 &&
        IsWritable(reinterpret_cast<void*>(view.matrixArray), static_cast<std::size_t>(view.count) * 0x120) &&
        IsWritable(reinterpret_cast<void*>(view.flagsArray), static_cast<std::size_t>(view.count) * sizeof(std::uint32_t));
}

extern "C" void __fastcall RetailMatrixCachePolicy(void* object, std::uint32_t index, const void* matrix)
{
    MatrixCacheView view{};
    bool valid = matrix && ReadMatrixCacheView(object, view) && IsValidMatrixCache(view, index);

    // Retail never constructs CameraManager's inline eye-1 cache at +0x470. The full stereo
    // camera job nevertheless calls its writer once per eye-1 pass. Construct that exact inline
    // sibling with the engine's real constructor; unlike a raw-array repair, this keeps
    // the matrix and dirty-flag vectors dynamically resizable by the engine.
    if constexpr (kConstructEye1MatrixCache)
    {
        if (!valid && matrix && g_stereoEnabled.load(std::memory_order_acquire) &&
            ReadTrueOutput() == 1 && object && IsWritable(object, 0x70))
        {
            MatrixCacheView primary{};
            const auto primaryObject = static_cast<std::uint8_t*>(object) - 0x260;
            const bool primaryValid = ReadMatrixCacheView(primaryObject, primary) &&
                IsValidMatrixCache(primary, index);
            std::uint32_t expected = 0;
            if (primaryValid && g_matrixCacheConstructor &&
                g_matrixCacheConstructionState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            {
                bool constructed = false;
                __try
                {
                    g_matrixCacheConstructor(object);
                    constructed = ReadMatrixCacheView(object, view) && IsValidMatrixCache(view, index);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    constructed = false;
                }
                g_matrixCacheConstructionState.store(constructed ? 2u : 3u, std::memory_order_release);
                if (constructed)
                {
                    g_matrixCacheConstructed.fetch_add(1, std::memory_order_relaxed);
                    valid = true;
                    Log("matrix-cache constructed eye-1 obj=%p idx=%u base=%p flags=%p count=%u current=%u",
                        object, index, reinterpret_cast<void*>(view.matrixArray),
                        reinterpret_cast<void*>(view.flagsArray), view.count, view.current);
                }
                else
                {
                    Log("matrix-cache eye-1 construction FAILED obj=%p idx=%u", object, index);
                }
            }
        }
    }
    if (!valid)
    {
        const auto skipped = g_matrixCacheSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipped <= 8)
            Log("matrix-cache skip #%llu obj=%p idx=%u base=%p flags=%p count=%u current=%u",
                static_cast<unsigned long long>(skipped), object, index,
                reinterpret_cast<void*>(view.matrixArray), reinterpret_cast<void*>(view.flagsArray),
                view.count, view.current);
        return;
    }
    g_matrixCachePassed.fetch_add(1, std::memory_order_relaxed);
    g_realMatrixCacheWriter(object, index, matrix);
}

bool ApplyHeadRotation(float* matrix, float yaw, float pitch, float roll)
{
    if (!matrix || !std::isfinite(yaw) || !std::isfinite(pitch) ||
        !std::isfinite(roll) ||
        std::fabs(matrix[15]) < 0.75f || std::fabs(matrix[15]) > 1.25f)
        return false;

    const float row0 = std::sqrt(matrix[0] * matrix[0] + matrix[1] * matrix[1] +
        matrix[2] * matrix[2]);
    const float row1 = std::sqrt(matrix[4] * matrix[4] + matrix[5] * matrix[5] +
        matrix[6] * matrix[6]);
    const float row2 = std::sqrt(matrix[8] * matrix[8] + matrix[9] * matrix[9] +
        matrix[10] * matrix[10]);
    if (row0 < 0.75f || row0 > 1.25f || row1 < 0.75f || row1 > 1.25f ||
        row2 < 0.75f || row2 > 1.25f)
        return false;

    const float tx = matrix[12];
    const float ty = matrix[13];
    const float tz = matrix[14];
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float c0x = matrix[0], c0y = matrix[4], c0z = matrix[8];
    const float c1x = matrix[1], c1y = matrix[5], c1z = matrix[9];
    const float c2x = matrix[2], c2y = matrix[6], c2z = matrix[10];
    const float eyeX = -(tx * c0x + ty * c1x + tz * c2x);
    const float eyeY = -(tx * c0y + ty * c1y + tz * c2y);
    const float eyeZ = -(tx * c0z + ty * c1z + tz * c2z);
    const float y0x = cy * c0x + sy * c2x;
    const float y0y = cy * c0y + sy * c2y;
    const float y0z = cy * c0z + sy * c2z;
    const float y2x = cy * c2x - sy * c0x;
    const float y2y = cy * c2y - sy * c0y;
    const float y2z = cy * c2z - sy * c0z;
    const float n1x = cp * c1x + sp * y2x;
    const float n1y = cp * c1y + sp * y2y;
    const float n1z = cp * c1z + sp * y2z;
    const float n2x = cp * y2x - sp * c1x;
    const float n2y = cp * y2y - sp * c1y;
    const float n2z = cp * y2z - sp * c1z;
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);
    const float r0x = cr * y0x + sr * n1x;
    const float r0y = cr * y0y + sr * n1y;
    const float r0z = cr * y0z + sr * n1z;
    const float r1x = cr * n1x - sr * y0x;
    const float r1y = cr * n1y - sr * y0y;
    const float r1z = cr * n1z - sr * y0z;

    matrix[0] = r0x; matrix[4] = r0y; matrix[8] = r0z;
    matrix[1] = r1x; matrix[5] = r1y; matrix[9] = r1z;
    matrix[2] = n2x; matrix[6] = n2y; matrix[10] = n2z;
    matrix[12] = -(eyeX * matrix[0] + eyeY * matrix[4] + eyeZ * matrix[8]);
    matrix[13] = -(eyeX * matrix[1] + eyeY * matrix[5] + eyeZ * matrix[9]);
    matrix[14] = -(eyeX * matrix[2] + eyeY * matrix[6] + eyeZ * matrix[10]);
    return true;
}

// [DECOUPLEPITCH] ("decoupled pitch", level-the-view
// form): take the game camera's own up/down tilt out before the head is
// applied, so only the head pitches the view.  FFXV is Y-up ([CAMSWAY] logs
// the camera right axis with y = 0).  Right axis kept, up axis = world +Y,
// forward flattened onto the ground plane (it stays perpendicular to right
// because right is already horizontal), eye position unchanged.  The stick
// still orbits the camera; only the view direction is levelled.
std::atomic_bool g_decoupledPitch{false};
bool LevelCameraPitch(float* matrix)
{
    const float c0x = matrix[0], c0y = matrix[4], c0z = matrix[8];
    const float c1x = matrix[1], c1y = matrix[5], c1z = matrix[9];
    const float c2x = matrix[2], c2y = matrix[6], c2z = matrix[10];
    const float tx = matrix[12], ty = matrix[13], tz = matrix[14];
    const float fLen = std::sqrt(c2x * c2x + c2z * c2z);
    const float rLen = std::sqrt(c0x * c0x + c0z * c0z);
    if (!std::isfinite(fLen) || fLen < 0.2f || rLen < 0.9f || c1y <= 0.0f)
        return false;   // looking almost straight up/down, rolled, or upside down
    const float eyeX = -(tx * c0x + ty * c1x + tz * c2x);
    const float eyeY = -(tx * c0y + ty * c1y + tz * c2y);
    const float eyeZ = -(tx * c0z + ty * c1z + tz * c2z);
    const float r0x = c0x / rLen, r0z = c0z / rLen;
    const float f2x = c2x / fLen, f2z = c2z / fLen;
    matrix[0] = r0x;  matrix[4] = 0.0f; matrix[8] = r0z;
    matrix[1] = 0.0f; matrix[5] = 1.0f; matrix[9] = 0.0f;
    matrix[2] = f2x;  matrix[6] = 0.0f; matrix[10] = f2z;
    matrix[12] = -(eyeX * matrix[0] + eyeY * matrix[4] + eyeZ * matrix[8]);
    matrix[13] = -(eyeX * matrix[1] + eyeY * matrix[5] + eyeZ * matrix[9]);
    matrix[14] = -(eyeX * matrix[2] + eyeY * matrix[6] + eyeZ * matrix[10]);
    return true;
}

bool ApplyCameraLocalOffset(float* matrix, float rightMeters, float upMeters,
    float forwardMeters)
{
    if (!matrix || !std::isfinite(rightMeters) || !std::isfinite(upMeters) ||
        !std::isfinite(forwardMeters) || std::fabs(rightMeters) > 8.0f ||
        std::fabs(upMeters) > 8.0f || std::fabs(forwardMeters) > 8.0f)
        return false;
    const float c0x = matrix[0], c0y = matrix[4], c0z = matrix[8];
    const float c1x = matrix[1], c1y = matrix[5], c1z = matrix[9];
    const float c2x = matrix[2], c2y = matrix[6], c2z = matrix[10];
    const float tx = matrix[12], ty = matrix[13], tz = matrix[14];
    float eyeX = -(tx * c0x + ty * c1x + tz * c2x);
    float eyeY = -(tx * c0y + ty * c1y + tz * c2y);
    float eyeZ = -(tx * c0z + ty * c1z + tz * c2z);
    eyeX += c0x * rightMeters + c1x * upMeters + c2x * forwardMeters;
    eyeY += c0y * rightMeters + c1y * upMeters + c2y * forwardMeters;
    eyeZ += c0z * rightMeters + c1z * upMeters + c2z * forwardMeters;
    matrix[12] = -(eyeX * c0x + eyeY * c0y + eyeZ * c0z);
    matrix[13] = -(eyeX * c1x + eyeY * c1y + eyeZ * c1z);
    matrix[14] = -(eyeX * c2x + eyeY * c2y + eyeZ * c2z);
    return true;
}

std::uint32_t* CameraParamTable();
float CamParamAsFloat(std::uint32_t raw);

// [VIEWCEN] Record one invocation of the camera hook.  Called with the game's
// own matrix, before any modification.
void RecordViewCensus(std::uintptr_t source, const float* matrix)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!matrix)
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    auto lastPresent = g_viewCenPresent.load(std::memory_order_relaxed);
    std::uint32_t index = 0;
    if (present != lastPresent)
    {
        // Frame boundary: the count just closed belongs to the previous frame.
        const auto closed = g_viewCenIndex.exchange(1, std::memory_order_acq_rel);
        g_viewCenPresent.store(present, std::memory_order_relaxed);
        if (closed > 0)
        {
            g_viewCenFrames.fetch_add(1, std::memory_order_relaxed);
            g_viewCenLastFrameCalls.store(closed, std::memory_order_relaxed);
            auto minSeen = g_viewCenPerFrameMin.load(std::memory_order_relaxed);
            while (closed < minSeen &&
                !g_viewCenPerFrameMin.compare_exchange_weak(minSeen, closed,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
            {
            }
            auto maxSeen = g_viewCenPerFrameMax.load(std::memory_order_relaxed);
            while (closed > maxSeen &&
                !g_viewCenPerFrameMax.compare_exchange_weak(maxSeen, closed,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
            {
            }
        }
        index = 0;
    }
    else
    {
        index = g_viewCenIndex.fetch_add(1, std::memory_order_acq_rel);
    }
    if (index >= kViewCenSlots)
        return;

    auto& slot = g_viewCensus[index];
    const float x = matrix[12];
    const float y = matrix[13];
    const float z = matrix[14];
    if (slot.seen.load(std::memory_order_relaxed))
    {
        const float dx = x - slot.lastPosition[0].load(std::memory_order_relaxed);
        const float dy = y - slot.lastPosition[1].load(std::memory_order_relaxed);
        const float dz = z - slot.lastPosition[2].load(std::memory_order_relaxed);
        const float delta = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (delta == delta)
        {
            slot.deltaSum.store(
                slot.deltaSum.load(std::memory_order_relaxed) + delta,
                std::memory_order_relaxed);
            if (delta > slot.deltaMax.load(std::memory_order_relaxed))
                slot.deltaMax.store(delta, std::memory_order_relaxed);
        }
    }
    slot.lastPosition[0].store(x, std::memory_order_relaxed);
    slot.lastPosition[1].store(y, std::memory_order_relaxed);
    slot.lastPosition[2].store(z, std::memory_order_relaxed);
    // Basis row 2 is the camera's forward axis; reflection views invert it.
    slot.lastForward[0].store(matrix[8], std::memory_order_relaxed);
    slot.lastForward[1].store(matrix[9], std::memory_order_relaxed);
    slot.lastForward[2].store(matrix[10], std::memory_order_relaxed);
    slot.seen.store(true, std::memory_order_relaxed);
    slot.samples.fetch_add(1, std::memory_order_relaxed);

    // Publish for the present path.  No logging here: this runs inside the
    // vectored exception handler for the camera breakpoint, where file I/O is
    // unsafe at this frequency.
    auto& live = g_viewCensusLive[index];
    live.position[0].store(x, std::memory_order_relaxed);
    live.position[1].store(y, std::memory_order_relaxed);
    live.position[2].store(z, std::memory_order_relaxed);
    live.forward[0].store(matrix[8], std::memory_order_relaxed);
    live.forward[1].store(matrix[9], std::memory_order_relaxed);
    live.forward[2].store(matrix[10], std::memory_order_relaxed);
    live.present.store(present, std::memory_order_relaxed);
}

// [VIEWCEN] Report from the present path, where logging is safe.
void ReportViewCensus()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto lastDump = g_viewCenLastDump.load(std::memory_order_relaxed);
    if (present - lastDump < 900)
        return;
    auto expected = lastDump;
    if (!g_viewCenLastDump.compare_exchange_strong(expected, present,
            std::memory_order_acq_rel, std::memory_order_relaxed))
        return;
    const auto frames = g_viewCenFrames.load(std::memory_order_relaxed);
    if (!frames)
        return;
    Log("[VIEWCEN] === summary frames=%llu callsPerFrame min=%u max=%u "
        "lastFrameCalls=%u present=%llu",
        static_cast<unsigned long long>(frames),
        g_viewCenPerFrameMin.load(std::memory_order_relaxed),
        g_viewCenPerFrameMax.load(std::memory_order_relaxed),
        g_viewCenLastFrameCalls.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(present));
    for (std::uint32_t probe = 0; probe < kViewCenSlots; ++probe)
    {
        auto& entry = g_viewCensus[probe];
        const auto samples = entry.samples.load(std::memory_order_relaxed);
        if (!samples)
            continue;
        const float meanDelta = samples > 1
            ? entry.deltaSum.load(std::memory_order_relaxed) /
                static_cast<float>(samples - 1) : 0.0f;
        auto& live = g_viewCensusLive[probe];
        Log("[VIEWCEN] call=%-2u samples=%llu meanDelta=%.4f maxDelta=%.2f "
            "pos=%.1f,%.1f,%.1f fwd=%.2f,%.2f,%.2f",
            probe, static_cast<unsigned long long>(samples), meanDelta,
            entry.deltaMax.load(std::memory_order_relaxed),
            live.position[0].load(std::memory_order_relaxed),
            live.position[1].load(std::memory_order_relaxed),
            live.position[2].load(std::memory_order_relaxed),
            live.forward[0].load(std::memory_order_relaxed),
            live.forward[1].load(std::memory_order_relaxed),
            live.forward[2].load(std::memory_order_relaxed));
        entry.deltaSum.store(0.0f, std::memory_order_relaxed);
        entry.deltaMax.store(0.0f, std::memory_order_relaxed);
        entry.samples.store(0, std::memory_order_relaxed);
    }
}


// [MODEPROBE] Read-only snapshot of what the game is actually doing this
// frame.  Called with the camera matrix this mod is about to commit, so the
// eye positions logged are the ones actually rendered.
void ProbeModeState(std::uintptr_t source, const float* matrix, int eye,
    bool aerActive)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!matrix || eye < 0 || eye > 1)
        return;
    // Eye separation is only meaningful within ONE view source; comparing
    // positions from different cameras gives values of 8-20 metres.
    EyePairCell* cell = nullptr;
    const auto key = source;
    const std::size_t start = (key >> 5) & (g_eyePairs.size() - 1);
    for (std::size_t probe = 0; probe < g_eyePairs.size(); ++probe)
    {
        auto& candidate = g_eyePairs[(start + probe) & (g_eyePairs.size() - 1)];
        auto existing = candidate.source.load(std::memory_order_acquire);
        if (existing == key)
        {
            cell = &candidate;
            break;
        }
        if (existing == 0 &&
            candidate.source.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            cell = &candidate;
            if (g_headSourceLogs.fetch_add(1, std::memory_order_relaxed) < 12)
                Log("[MODEPROBE] new view source %llu total sources seen",
                    static_cast<unsigned long long>(
                        g_headSourceLogs.load(std::memory_order_relaxed)));
            break;
        }
    }
    if (!cell)
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    for (int axis = 0; axis < 3; ++axis)
        cell->position[eye][axis].store(matrix[12 + axis],
            std::memory_order_relaxed);
    cell->present[eye].store(present, std::memory_order_relaxed);
    cell->seen.fetch_or(1u << eye, std::memory_order_relaxed);
    if (cell->seen.load(std::memory_order_relaxed) != 3u)
        return;

    // Measure the PAIR, on the frame that closes it.  Sampling on eye-0
    // frames would compare the value just written against the previous
    // pair's eye-1 value (two frames and a fresh capture apart), and an
    // even-length interval locks to one eye parity.
    if (eye != 1)
        return;
    const auto otherPresent = cell->present[0].load(std::memory_order_relaxed);
    if (present - otherPresent != 1)
        return;

    auto& state = g_modeProbe;
    const auto lastPresent = state.lastLogPresent.load(std::memory_order_relaxed);
    if (present - lastPresent < 121)
        return;
    auto expected = lastPresent;
    if (!state.lastLogPresent.compare_exchange_strong(expected, present,
            std::memory_order_acq_rel, std::memory_order_relaxed))
        return;

    float delta[3]{};
    for (int axis = 0; axis < 3; ++axis)
        delta[axis] = cell->position[0][axis].load(std::memory_order_relaxed) -
            cell->position[1][axis].load(std::memory_order_relaxed);
    const float separation = std::sqrt(
        delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);

    if (state.logs.fetch_add(1, std::memory_order_relaxed) > 3000)
        return;
    Log("[MODEPROBE] PAIR eyeSep=%.4fm expected=%.4fm hold=%d held=%d eye=%d "
        "aer=%d camPos=%.2f,%.2f,%.2f present=%llu",
        separation, g_aerHalfEyeMeters.load(std::memory_order_relaxed) * 2.0f,
        g_eyeHoldEnabled.load(std::memory_order_relaxed) ? 1 : 0,
        g_eyeHoldAppliedThisFrame.load(std::memory_order_relaxed) ? 1 : 0,
        eye, aerActive ? 1 : 0,
        matrix[12], matrix[13], matrix[14],
        static_cast<unsigned long long>(present));
}

bool ApplyAerEyeOffset(float* matrix, float offsetMeters)
{
    if (!matrix || !std::isfinite(offsetMeters) || std::fabs(offsetMeters) > 0.5f)
        return false;
    const float c0x = matrix[0], c0y = matrix[4], c0z = matrix[8];
    const float c1x = matrix[1], c1y = matrix[5], c1z = matrix[9];
    const float c2x = matrix[2], c2y = matrix[6], c2z = matrix[10];
    const float tx = matrix[12], ty = matrix[13], tz = matrix[14];
    float eyeX = -(tx * c0x + ty * c1x + tz * c2x);
    float eyeY = -(tx * c0y + ty * c1y + tz * c2y);
    float eyeZ = -(tx * c0z + ty * c1z + tz * c2z);
    eyeX += c0x * offsetMeters;
    eyeY += c0y * offsetMeters;
    eyeZ += c0z * offsetMeters;
    matrix[12] = -(eyeX * c0x + eyeY * c0y + eyeZ * c0z);
    matrix[13] = -(eyeX * c1x + eyeY * c1y + eyeZ * c1z);
    matrix[14] = -(eyeX * c2x + eyeY * c2y + eyeZ * c2z);
    return true;
}
// World-space eye position of a view matrix (the inverse of the translation
// ApplyAerEyeOffset rebuilds).
void ViewEyePosition(const float* matrix, float* eye)
{
    const float tx = matrix[12], ty = matrix[13], tz = matrix[14];
    eye[0] = -(tx * matrix[0] + ty * matrix[1] + tz * matrix[2]);
    eye[1] = -(tx * matrix[4] + ty * matrix[5] + tz * matrix[6]);
    eye[2] = -(tx * matrix[8] + ty * matrix[9] + tz * matrix[10]);
}

bool IsDynamicHeadMatrix(std::uintptr_t source)
{
    if (source < 0x10000 || source > 0x00007FFFFFFFFFFFull ||
        !IsReadable(reinterpret_cast<const void*>(source), sizeof(float) * 16))
        return false;
    __try
    {
        const auto* matrix = reinterpret_cast<const float*>(source);
        if (!std::isfinite(matrix[15]) || std::fabs(matrix[15]) < 0.75f ||
            std::fabs(matrix[15]) > 1.25f)
            return false;
        for (std::size_t row = 0; row < 3; ++row)
        {
            const auto i = row * 4;
            const float length = std::sqrt(matrix[i] * matrix[i] +
                matrix[i + 1] * matrix[i + 1] + matrix[i + 2] * matrix[i + 2]);
            if (!std::isfinite(length) || length < 0.75f || length > 1.25f)
                return false;
        }
        return std::fabs(matrix[12]) < 100000.0f &&
            std::fabs(matrix[13]) < 100000.0f && std::fabs(matrix[14]) < 100000.0f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool DynamicHeadPatternAt(std::uintptr_t address, const std::uint8_t* pattern,
    std::size_t size)
{
    if (!address || !pattern || !size ||
        !IsReadable(reinterpret_cast<const void*>(address), size))
        return false;
    __try
    {
        return std::memcmp(reinterpret_cast<const void*>(address), pattern, size) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::uintptr_t FindDynamicHeadPattern(const std::uint8_t* pattern, std::size_t size,
    std::uintptr_t after = 0)
{
    if (!g_exeBase || !pattern || !size)
        return 0;
    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            g_exeBase + dos->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        const auto imageEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
        for (std::uint16_t index = 0; index < nt->FileHeader.NumberOfSections;
            ++index, ++section)
        {
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            auto begin = g_exeBase + section->VirtualAddress;
            const auto span = section->Misc.VirtualSize > section->SizeOfRawData
                ? section->Misc.VirtualSize : section->SizeOfRawData;
            auto end = begin + span;
            if (end > imageEnd)
                end = imageEnd;
            if (after >= begin && after < end)
                begin = after + 1;
            if (begin >= end || static_cast<std::size_t>(end - begin) < size)
                continue;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(begin);
            const auto count = static_cast<std::size_t>(end - begin) - size + 1;
            for (std::size_t offset = 0; offset < count; ++offset)
            {
                if (bytes[offset] == pattern[0] &&
                    std::memcmp(bytes + offset, pattern, size) == 0)
                    return begin + offset;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return 0;
}

std::uintptr_t ResolveDynamicHeadCaptureSite()
{
    constexpr std::uintptr_t kKnownLatestGetterCallRva = 0x01073C7B;
    constexpr std::array<std::uint8_t, 7> kGetterA = {
        0xFF, 0x50, 0x28, 0x44, 0x0F, 0x28, 0x38,
    };
    const auto knownCall = g_exeBase + kKnownLatestGetterCallRva;
    if (DynamicHeadPatternAt(knownCall, kGetterA.data(), kGetterA.size()))
    {
        Log("[VRHEAD] dynamic getter-A resolved at known current-build site +0x%llX",
            static_cast<unsigned long long>(kKnownLatestGetterCallRva + 3));
        return knownCall + 3;
    }

    constexpr std::array<std::uint8_t, 20> kGetterCFrustum = {
        0xFF, 0x50, 0x38, 0x0F, 0x28, 0x30, 0x0F, 0x28, 0x78, 0x10,
        0x44, 0x0F, 0x28, 0x40, 0x20, 0x44, 0x0F, 0x28, 0x48, 0x30,
    };
    std::uintptr_t after = 0;
    for (unsigned hit = 0; hit < 4; ++hit)
    {
        const auto anchor = FindDynamicHeadPattern(
            kGetterCFrustum.data(), kGetterCFrustum.size(), after);
        if (!anchor)
            break;
        after = anchor;
        const auto imageEnd = g_exeBase + kRetailImageSize;
        const auto begin = anchor >= g_exeBase + 0x260 ? anchor - 0x260 : g_exeBase;
        const auto end = anchor + 0x180 < imageEnd ? anchor + 0x180 : imageEnd;
        for (auto address = begin; address + kGetterA.size() <= end; ++address)
        {
            if (DynamicHeadPatternAt(address, kGetterA.data(), kGetterA.size()))
            {
                Log("[VRHEAD] dynamic getter-A pattern resolved anchor=+0x%llX site=+0x%llX",
                    static_cast<unsigned long long>(anchor - g_exeBase),
                    static_cast<unsigned long long>(address + 3 - g_exeBase));
                return address + 3;
            }
        }
    }
    return 0;
}

std::uint32_t SetDynamicHeadBreakpointAllThreads(
    std::uintptr_t address, bool enable, bool execute)
{
    if (!address)
        return 0;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();
    std::uint32_t applied = 0;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            const auto thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            DebugRegisterUpdates::Guard registerUpdate;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context))
                {
                    context.Dr7 &= ~(0xFull << 16);
                    context.Dr7 &= ~0x3ull;
                    if (enable)
                    {
                        context.Dr0 = address;
                        if (!execute)
                            context.Dr7 |= (0xDull << 16); // write, four bytes
                        context.Dr7 |= 0x1ull;
                    }
                    else
                    {
                        context.Dr0 = 0;
                    }
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context))
                        ++applied;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return applied;
}

std::uint32_t SetFovSetterBreakpointAllThreads(bool enable)
{
    if (!g_exeBase)
        return 0;
    const auto address = g_exeBase + kFovSetterRva;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();
    std::uint32_t applied = 0;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            const auto thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            DebugRegisterUpdates::Guard registerUpdate;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context))
                {
                    context.Dr7 &= ~(0xFull << 20);
                    context.Dr7 &= ~0xCull;
                    if (enable)
                    {
                        context.Dr1 = address;
                        // condition 00 (execute), length 00 already cleared
                        context.Dr7 |= 0x4ull;
                    }
                    else
                    {
                        context.Dr1 = 0;
                    }
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context))
                        ++applied;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (enable)
        g_fovSetHits.store(0, std::memory_order_relaxed);
    g_fovSetArmed.store(enable && applied, std::memory_order_release);
    Log("[FOVSET] setter breakpoint %s on %u threads (rva +0x%llX)",
        enable ? "ARMED" : "disarmed", applied,
        static_cast<unsigned long long>(kFovSetterRva));
    return applied;
}

std::uintptr_t ResolveInteractionExecute()
{
    if (!g_exeBase)
        return 0;
    const auto slot = g_exeBase + kSeqActionInteractionVtableRva + kSeqExecuteSlotOffset;
    if (!IsReadable(reinterpret_cast<const void*>(slot), sizeof(std::uintptr_t)))
        return 0;
    std::uintptr_t function = 0;
    __try
    {
        function = *reinterpret_cast<const std::uintptr_t*>(slot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    if (function < g_exeBase || function >= g_exeBase + kRetailImageSize)
        return 0;
    return function;
}

// [ISEQ] DR3 execute breakpoint on SequenceActionInteraction::execute, every
// thread.  Same VEH resume pattern as the DR0 head hook and DR1 FOV hook.
std::uint32_t SetInteractionWatchAllThreads(bool enable)
{
    const auto address = ResolveInteractionExecute();
    if (enable && !address)
    {
        Log("[ISEQ] SequenceActionInteraction::execute not resolvable; watch not armed");
        return 0;
    }
    if (enable)
        g_iseqExecuteAddress.store(address, std::memory_order_release);
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();
    std::uint32_t applied = 0;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            const auto thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            DebugRegisterUpdates::Guard registerUpdate;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context))
                {
                    context.Dr7 &= ~(0xFull << 28); // RW3/LEN3 = execute, 1 byte
                    context.Dr7 &= ~0xC0ull;         // L3/G3
                    if (enable)
                    {
                        context.Dr3 = address;
                        context.Dr7 |= 0x40ull;      // L3
                    }
                    else
                        context.Dr3 = 0;
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context))
                        ++applied;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    g_iseqArmed.store(enable && applied, std::memory_order_release);
    Log("[ISEQ] interaction execute watch %s on %u threads (execute=+0x%llX vtable=+0x%llX)",
        enable ? "ARMED" : "disarmed", applied,
        static_cast<unsigned long long>(address ? address - g_exeBase : 0),
        static_cast<unsigned long long>(kSeqActionInteractionVtableRva));
    return applied;
}

// [SEQCENSUS] Find the tick's indirect execute call: `call [reg+0x130]`
// (FF /2 with a disp32 of 0x130, optionally REX-prefixed) inside the tick.
std::uintptr_t ResolveSequenceExecuteCallSite()
{
    if (!g_exeBase)
        return 0;
    const auto tick = g_exeBase + kSequenceTickRva;
    if (!IsReadable(reinterpret_cast<const void*>(tick), kSequenceTickScanBytes))
    {
        Log("[SEQCENSUS] sequence tick +0x%llX not readable",
            static_cast<unsigned long long>(kSequenceTickRva));
        return 0;
    }
    std::uintptr_t found = 0;
    std::uint32_t candidates = 0;
    __try
    {
        const auto* code = reinterpret_cast<const std::uint8_t*>(tick);
        for (std::size_t i = 0; i + 6 <= kSequenceTickScanBytes; ++i)
        {
            std::size_t at = i;
            if (code[at] >= 0x40 && code[at] <= 0x4F)
                ++at; // REX
            if (at + 6 > kSequenceTickScanBytes || code[at] != 0xFF)
                continue;
            const std::uint8_t modrm = code[at + 1];
            // mod=10 (disp32), reg=/2 (call), rm != 100 (no SIB)
            if ((modrm & 0xF8) != 0x90 || (modrm & 0x07) == 0x04)
                continue;
            if (*reinterpret_cast<const std::uint32_t*>(code + at + 2) != 0x130u)
                continue;
            ++candidates;
            Log("[SEQCENSUS] candidate call [r%u+0x130] at tick+0x%zX (rex=%d)",
                static_cast<unsigned>(modrm & 0x07), i, at != i ? 1 : 0);
            if (!found)
                found = tick + i;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    Log("[SEQCENSUS] %u candidate execute call sites in the sequence tick", candidates);
    return found;
}

// DR2 execute breakpoint on the tick's execute call, every thread.
std::uint32_t SetSequenceCensusAllThreads(bool enable)
{
    static std::uintptr_t resolved = 0;
    static bool tried = false;
    if (enable && !tried)
    {
        tried = true;
        resolved = ResolveSequenceExecuteCallSite();
    }
    const auto address = resolved;
    if (enable && !address)
    {
        Log("[SEQCENSUS] execute call site not resolvable; census not armed");
        return 0;
    }
    // The address stays published after a disarm: a thread that hits DR2
    // between this store and its register clear must still be resumed by the
    // handler (a crash 86 ms after a disarm came from that gap).
    if (enable)
        g_seqCensusCallSite.store(address, std::memory_order_release);
    g_seqCensusArmed.store(false, std::memory_order_release);
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();
    std::uint32_t applied = 0;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            const auto thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            DebugRegisterUpdates::Guard registerUpdate;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context))
                {
                    context.Dr7 &= ~(0xFull << 24); // RW2/LEN2 = execute, 1 byte
                    context.Dr7 &= ~0x30ull;         // L2/G2
                    if (enable)
                    {
                        context.Dr2 = address;
                        context.Dr7 |= 0x10ull;      // L2
                    }
                    else
                        context.Dr2 = 0;
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context))
                        ++applied;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    g_seqCensusArmed.store(enable && applied, std::memory_order_release);
    Log("[SEQCENSUS] action census %s on %u threads (call site=+0x%llX)",
        enable ? "ARMED" : "disarmed", applied,
        static_cast<unsigned long long>(address ? address - g_exeBase : 0));
    return applied;
}

// MSVC RTTI: vtable[-1] -> RTTICompleteObjectLocator {sig, off, cdOff,
// typeDescRva, classDescRva, selfRva}; TypeDescriptor name at +0x10.
bool SeqCensusClassName(std::uintptr_t vtableRva, char* out, std::size_t size)
{
    out[0] = 0;
    __try
    {
        const auto vtable = g_exeBase + vtableRva;
        if (!IsReadable(reinterpret_cast<const void*>(vtable - 8), 8))
            return false;
        const auto locator = *reinterpret_cast<const std::uintptr_t*>(vtable - 8);
        if (!IsReadable(reinterpret_cast<const void*>(locator), 0x18))
            return false;
        const auto* col = reinterpret_cast<const std::uint32_t*>(locator);
        const auto typeRva = col[3];
        if (col[0] != 1 || !typeRva || typeRva >= kRetailImageSize)
            return false;
        const auto* name = reinterpret_cast<const char*>(g_exeBase + typeRva + 0x10);
        if (!IsReadable(name, 160))
            return false;
        std::size_t n = 0;
        for (; n + 1 < size && n < 159 && name[n]; ++n)
            out[n] = name[n];
        out[n] = 0;
        return n > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out[0] = 0;
        return false;
    }
}

void AiWaitRecord(std::uintptr_t self, std::uintptr_t vtableRva, std::uint64_t present)
{
    AiWaitSlot* target = nullptr;
    for (auto& slot : g_aiWaits)
    {
        if (slot.self.load(std::memory_order_acquire) == self)
        {
            target = &slot;
            break;
        }
    }
    if (!target)
    {
        const bool released = g_seqCensusReleasePresent.load(std::memory_order_relaxed) != 0;
        for (auto& slot : g_aiWaits)
        {
            auto existing = slot.self.load(std::memory_order_acquire);
            const bool stale = existing && !released &&
                present - slot.lastPresent.load(std::memory_order_relaxed) > 600;
            if ((!existing || stale) && slot.self.compare_exchange_strong(existing, self,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                slot.vtableRva.store(vtableRva, std::memory_order_relaxed);
                slot.hits.store(0, std::memory_order_relaxed);
                slot.firstPresent.store(present, std::memory_order_relaxed);
                slot.haveReleaseSnap = false;
                target = &slot;
                break;
            }
        }
    }
    if (!target)
        return;
    target->hits.fetch_add(1, std::memory_order_relaxed);
    target->lastPresent.store(present, std::memory_order_relaxed);
    __try
    {
        if (IsReadable(reinterpret_cast<const void*>(self), kAiSnapBytes))
        {
            std::memcpy(target->snap, reinterpret_cast<const void*>(self), kAiSnapBytes);
            // [ACTORDIFF] publish the target actor of a PlayMotion that has
            // been waiting (status 0) for at least 120 presents.
            if (vtableRva == kExecAIModePlayMotionVtableRva)
            {
                std::int32_t status, count;
                std::uint64_t vec;
                std::memcpy(&status, target->snap + 0x480, 4);
                std::memcpy(&count, target->snap + 0x490, 4);
                std::memcpy(&vec, target->snap + 0x488, 8);
                const auto first = target->firstPresent.load(std::memory_order_relaxed);
                if (status == 0 && count >= 1 && vec && present - first >= 120 &&
                    IsReadable(reinterpret_cast<const void*>(vec), 16))
                {
                    std::uint64_t actor;
                    std::memcpy(&actor, reinterpret_cast<const void*>(vec + 8), 8);
                    if (actor && g_stuckActorNode.load(std::memory_order_relaxed) != self)
                    {
                        g_stuckActor.store(static_cast<std::uintptr_t>(actor),
                            std::memory_order_relaxed);
                        g_stuckActorSince.store(first, std::memory_order_relaxed);
                        g_stuckActorNode.store(self, std::memory_order_release);
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Present thread.  Snapshot + diff of the stuck PlayMotion's actor.
void ActorDiffTick(const char* tag, std::uint64_t present, std::uint64_t release)
{
    const auto actor = g_stuckActor.load(std::memory_order_acquire);
    if (!actor)
        return;
    static std::uint8_t current[kActorSnapBytes];
    bool ok = false;
    __try
    {
        if (IsReadable(reinterpret_cast<const void*>(actor), kActorSnapBytes))
        {
            std::memcpy(current, reinterpret_cast<const void*>(actor), kActorSnapBytes);
            ok = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    if (!ok)
    {
        Log("[ACTORDIFF] %s actor=%llX not readable", tag, static_cast<unsigned long long>(actor));
        return;
    }
    if (g_actorSnapTarget != actor)
    {
        g_actorSnapTarget = actor;
        g_actorSnapHavePrev = false;
        g_actorSnapHaveRelease = false;
        std::uint64_t vtable = 0;
        std::memcpy(&vtable, current, 8);
        char name[160] = "?";
        if (vtable >= g_exeBase && vtable < g_exeBase + kRetailImageSize)
            SeqCensusClassName(vtable - g_exeBase, name, sizeof(name));
        Log("[ACTORDIFF] tracking actor=%llX class=%s (vtable +%llX) node=%llX waiting since present %llu",
            static_cast<unsigned long long>(actor), name,
            static_cast<unsigned long long>(vtable >= g_exeBase ? vtable - g_exeBase : 0),
            static_cast<unsigned long long>(g_stuckActorNode.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_stuckActorSince.load(std::memory_order_relaxed)));
        // Measured: the node's target is an ActorHandle, not the actor, and
        // 0x2000 bytes ran into neighbouring heap objects.  Name every object
        // pointer inside the handle's first 0x180 bytes so the one that is
        // the character can be identified.
        for (std::size_t o = 0; o + 8 <= 0x180; o += 8)
        {
            std::uint64_t p;
            std::memcpy(&p, current + o, 8);
            if (p < 0x10000 || p >= 0x00007FF000000000ull)
                continue;
            std::uint64_t pv = 0;
            __try
            {
                if (IsReadable(reinterpret_cast<const void*>(p), 8))
                    std::memcpy(&pv, reinterpret_cast<const void*>(p), 8);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                pv = 0;
            }
            if (pv < g_exeBase || pv >= g_exeBase + kRetailImageSize)
                continue;
            char pname[160] = "";
            SeqCensusClassName(pv - g_exeBase, pname, sizeof(pname));
            Log("[ACTORDIFF]   handle+0x%03zX -> %llX %s", o, static_cast<unsigned long long>(p),
                pname[0] ? pname : "(object, no RTTI)");
            if (std::strstr(pname, "ActorCharacter") && !g_charTarget.load(std::memory_order_relaxed))
            {
                g_charTarget.store(static_cast<std::uintptr_t>(p), std::memory_order_release);
                g_charWatchWritten.store(0, std::memory_order_release);
                g_charWatchLogged.store(0, std::memory_order_relaxed);
                if (const auto worker = CreateThread(nullptr, 0, &CharWatchArmThread,
                        reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(p) + kCharWatchOffset),
                        0, nullptr))
                    CloseHandle(worker);
                Log("[CHARDIFF] character = %llX (from handle+0x%03zX)",
                    static_cast<unsigned long long>(p), o);
            }
        }
    }
    auto diffLine = [&](const char* kind, const std::uint8_t* base) {
        char line[1800];
        int n = std::snprintf(line, sizeof(line), "[ACTORDIFF] %s %s actor=%llX:", tag, kind,
            static_cast<unsigned long long>(actor));
        std::uint32_t changed = 0;
        for (std::size_t o = 8; o + 4 <= kActorSnapBytes; o += 4)
        {
            std::uint32_t a, b;
            std::memcpy(&a, base + o, 4);
            std::memcpy(&b, current + o, 4);
            if (a == b)
                continue;
            ++changed;
            if (n < static_cast<int>(sizeof(line)) - 40)
                n += std::snprintf(line + n, sizeof(line) - n, " +%zX:%X>%X", o, a, b);
        }
        Log("%s  [%u dwords]", line, changed);
    };
    if (g_actorSnapHavePrev)
        diffLine("vs-prev", g_actorSnapPrev);
    if (release && g_actorSnapHaveRelease)
        diffLine("vs-release", g_actorSnapRelease);
    std::memcpy(g_actorSnapPrev, current, kActorSnapBytes);
    g_actorSnapHavePrev = true;
    if (release && !g_actorSnapHaveRelease)
    {
        std::memcpy(g_actorSnapRelease, current, kActorSnapBytes);
        g_actorSnapHaveRelease = true;
    }
}

void AiWaitReport(const char* tag, std::uint64_t present, std::uint64_t release)
{
    for (auto& slot : g_aiWaits)
    {
        const auto self = slot.self.load(std::memory_order_acquire);
        if (!self)
            continue;
        const auto last = slot.lastPresent.load(std::memory_order_relaxed);
        if (present - last > 120 && !(release && last + 120 >= release))
            continue;
        std::uint8_t s[kAiSnapBytes];
        std::memcpy(s, slot.snap, kAiSnapBytes);
        auto I = [&](std::size_t o) { std::int32_t v; std::memcpy(&v, s + o, 4); return v; };
        auto Q = [&](std::size_t o) { std::uint64_t v; std::memcpy(&v, s + o, 8); return v; };
        // first two target entries (16 bytes each) of the resolved vector
        std::uint64_t t0 = 0, t1 = 0, t2 = 0;
        const auto vec = Q(0x488);
        __try
        {
            if (vec && IsReadable(reinterpret_cast<const void*>(vec), 32))
            {
                std::memcpy(&t0, reinterpret_cast<const void*>(vec), 8);
                std::memcpy(&t1, reinterpret_cast<const void*>(vec + 8), 8);
                std::memcpy(&t2, reinterpret_cast<const void*>(vec + 16), 8);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        Log("[AIWAIT] %s inst=%llX class=%s hits=%llu first=%llu last=%+lld | status480=%d "
            "count490=%d vec488=%llX t0=%llX/%llX t1=%llX | ref168=%llX/%llX state40=%d flag45=%u",
            tag, static_cast<unsigned long long>(self),
            slot.vtableRva.load(std::memory_order_relaxed) == kExecAIModeWaitVtableRva ? "Wait"
                                                                                       : "PlayMotion",
            static_cast<unsigned long long>(slot.hits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(slot.firstPresent.load(std::memory_order_relaxed)),
            static_cast<long long>(last) - static_cast<long long>(release ? release : present),
            I(0x480), I(0x490), static_cast<unsigned long long>(vec),
            static_cast<unsigned long long>(t0), static_cast<unsigned long long>(t1),
            static_cast<unsigned long long>(t2),
            static_cast<unsigned long long>(Q(0x168)), static_cast<unsigned long long>(Q(0x170)),
            I(0x40), static_cast<unsigned>(s[0x45]));
    }
}

void AiWaitMarkRelease()
{
    for (auto& slot : g_aiWaits)
    {
        if (!slot.self.load(std::memory_order_acquire))
            continue;
        std::memcpy(slot.releaseSnap, slot.snap, kAiSnapBytes);
        slot.haveReleaseSnap = true;
    }
}

void AiWaitReleaseDiff(std::uint64_t present, std::uint64_t release)
{
    for (auto& slot : g_aiWaits)
    {
        const auto self = slot.self.load(std::memory_order_acquire);
        if (!self || !slot.haveReleaseSnap)
            continue;
        std::uint8_t s[kAiSnapBytes];
        std::memcpy(s, slot.snap, kAiSnapBytes);
        char line[1600];
        int n = std::snprintf(line, sizeof(line), "[AIWAIT] diff inst=%llX release->+%llu:",
            static_cast<unsigned long long>(self),
            static_cast<unsigned long long>(present - release));
        std::uint32_t changed = 0;
        for (std::size_t o = 8; o + 4 <= kAiSnapBytes; o += 4)
        {
            std::uint32_t a, b;
            std::memcpy(&a, slot.releaseSnap + o, 4);
            std::memcpy(&b, s + o, 4);
            if (a == b)
                continue;
            ++changed;
            if (n < static_cast<int>(sizeof(line)) - 40)
                n += std::snprintf(line + n, sizeof(line) - n, " +%zX:%X->%X", o, a, b);
        }
        Log("%s  [%u dwords]", line, changed);
    }
}

// [SEQACT] sequence activity meter: how many action instances were first
// seen / went quiet in the last window.  A stalled sequence shows ~0 new.
void SeqActivityReport(std::uint64_t present, std::uint64_t window)
{
    std::uint32_t fresh = 0, quiet = 0, running = 0;
    for (auto& slot : g_seqInst)
    {
        if (!slot.self.load(std::memory_order_acquire))
            continue;
        const auto first = slot.firstPresent.load(std::memory_order_relaxed);
        const auto last = slot.lastPresent.load(std::memory_order_relaxed);
        if (first + window > present)
            ++fresh;
        if (last + 10 >= present)
            ++running;
        else if (last + window > present)
            ++quiet;
    }
    Log("[SEQACT] present=%llu window=%llu newInstances=%u wentQuiet=%u running=%u",
        static_cast<unsigned long long>(present), static_cast<unsigned long long>(window),
        fresh, quiet, running);
}

void TimelineRecord(std::uintptr_t self, std::uint64_t present)
{
    TimelineSlot* target = nullptr;
    for (auto& slot : g_timelines)
    {
        if (slot.self.load(std::memory_order_acquire) == self)
        {
            target = &slot;
            break;
        }
    }
    if (!target)
    {
        const bool released = g_seqCensusReleasePresent.load(std::memory_order_relaxed) != 0;
        for (auto& slot : g_timelines)
        {
            auto existing = slot.self.load(std::memory_order_acquire);
            const bool stale = existing && !released &&
                present - slot.lastPresent.load(std::memory_order_relaxed) > 600;
            if ((!existing || stale) && slot.self.compare_exchange_strong(existing, self,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                slot.hits.store(0, std::memory_order_relaxed);
                slot.firstPresent.store(present, std::memory_order_relaxed);
                slot.haveReleaseSnap = false;
                target = &slot;
                break;
            }
        }
    }
    if (!target)
        return;
    target->hits.fetch_add(1, std::memory_order_relaxed);
    target->lastPresent.store(present, std::memory_order_relaxed);
    __try
    {
        if (IsReadable(reinterpret_cast<const void*>(self), kTlSnapBytes))
        {
            std::memcpy(target->snap, reinterpret_cast<const void*>(self), kTlSnapBytes);
            target->seq.fetch_add(1, std::memory_order_relaxed);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void TimelineReport(const char* tag, std::uint64_t present, std::uint64_t release)
{
    for (auto& slot : g_timelines)
    {
        const auto self = slot.self.load(std::memory_order_acquire);
        if (!self)
            continue;
        const auto last = slot.lastPresent.load(std::memory_order_relaxed);
        if (present - last > 120 && !(release && last + 120 >= release))
            continue;
        std::uint8_t s[kTlSnapBytes];
        std::memcpy(s, slot.snap, kTlSnapBytes);
        auto I = [&](std::size_t o) { std::int32_t v; std::memcpy(&v, s + o, 4); return v; };
        auto F = [&](std::size_t o) { float v; std::memcpy(&v, s + o, 4); return v; };
        auto B = [&](std::size_t o) { return static_cast<unsigned>(s[o]); };
        Log("[TLPROBE] %s inst=%llX hits=%llu first=%llu last=%+lld | state=%d time=%.3f "
            "speed=%.3f frames108=%u pre130=%u stop131=%u preT=%.3f preEnd=%.3f | ready575=%u "
            "skip576=%u paused57D=%u end57E=%u loops=%d | wait584=%u w585=%u waitId588=%d "
            "mode590=%d | extClock55C=%u clockScale=%.3f loop554=%u b58C=%u id5F4=%d",
            tag, static_cast<unsigned long long>(self),
            static_cast<unsigned long long>(slot.hits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(slot.firstPresent.load(std::memory_order_relaxed)),
            static_cast<long long>(last) - static_cast<long long>(release ? release : present),
            I(0x550), F(0x578), F(0x138), B(0x108), B(0x130), B(0x131), F(0x134), F(0x56C),
            B(0x575), B(0x576), B(0x57D), B(0x57E), I(0x580), B(0x584), B(0x585), I(0x588),
            I(0x590), B(0x55C), F(0x558), B(0x554), B(0x58C), I(0x5F4));
    }
}

void TimelineMarkRelease()
{
    for (auto& slot : g_timelines)
    {
        if (!slot.self.load(std::memory_order_acquire))
            continue;
        std::memcpy(slot.releaseSnap, slot.snap, kTlSnapBytes);
        slot.haveReleaseSnap = true;
    }
}

void TimelineReleaseDiff(std::uint64_t present, std::uint64_t release)
{
    for (auto& slot : g_timelines)
    {
        const auto self = slot.self.load(std::memory_order_acquire);
        if (!self || !slot.haveReleaseSnap)
            continue;
        std::uint8_t s[kTlSnapBytes];
        std::memcpy(s, slot.snap, kTlSnapBytes);
        char line[1600];
        int n = std::snprintf(line, sizeof(line), "[TLPROBE] diff inst=%llX release->+%llu:",
            static_cast<unsigned long long>(self),
            static_cast<unsigned long long>(present - release));
        std::uint32_t changed = 0;
        for (std::size_t o = 8; o + 4 <= kTlSnapBytes; o += 4)
        {
            std::uint32_t a, b;
            std::memcpy(&a, slot.releaseSnap + o, 4);
            std::memcpy(&b, s + o, 4);
            if (a == b)
                continue;
            ++changed;
            if (n < static_cast<int>(sizeof(line)) - 48)
            {
                float fb;
                std::memcpy(&fb, &b, 4);
                n += std::snprintf(line + n, sizeof(line) - n, " +%zX:%X->%X(%.3g)", o, a, b, fb);
            }
        }
        Log("%s  [%u dwords]", line, changed);
    }
}

void SeqInstRecord(std::uintptr_t self, std::uintptr_t vtableRva, std::uint64_t present)
{
    const bool released = g_seqCensusReleasePresent.load(std::memory_order_relaxed) != 0;
    std::size_t index = static_cast<std::size_t>((self >> 4) * 0x9E3779B97F4A7C15ull >> 50) &
        (kSeqInstSlots - 1);
    for (std::size_t probe = 0; probe < 96; ++probe, index = (index + 1) & (kSeqInstSlots - 1))
    {
        auto& slot = g_seqInst[index];
        auto existing = slot.self.load(std::memory_order_acquire);
        if (existing == self)
        {
            slot.hits.fetch_add(1, std::memory_order_relaxed);
            slot.lastPresent.store(present, std::memory_order_relaxed);
            if (released)
                slot.sinceRelease.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool stale = existing != 0 && !released &&
            present - slot.lastPresent.load(std::memory_order_relaxed) > kSeqInstStalePresents;
        if ((!existing || stale) && slot.self.compare_exchange_strong(existing, self,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            slot.vtableRva.store(vtableRva, std::memory_order_relaxed);
            slot.hits.store(1, std::memory_order_relaxed);
            slot.firstPresent.store(present, std::memory_order_relaxed);
            slot.lastPresent.store(present, std::memory_order_relaxed);
            slot.sinceRelease.store(released ? 1 : 0, std::memory_order_relaxed);
            slot.activeAtRelease.store(false, std::memory_order_relaxed);
            return;
        }
    }
    g_seqInstOverflow.fetch_add(1, std::memory_order_relaxed);
}

void SeqCensusRecord(std::uintptr_t vtableRva, std::uint64_t present)
{
    for (auto& slot : g_seqCensus)
    {
        auto existing = slot.vtableRva.load(std::memory_order_acquire);
        if (existing == vtableRva)
        {
            slot.hits.fetch_add(1, std::memory_order_relaxed);
            slot.windowHits.fetch_add(1, std::memory_order_relaxed);
            slot.lastPresent.store(present, std::memory_order_relaxed);
            if (g_seqCensusReleasePresent.load(std::memory_order_relaxed))
                slot.sinceRelease.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!existing && slot.vtableRva.compare_exchange_strong(existing, vtableRva,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            slot.hits.store(1, std::memory_order_relaxed);
            slot.windowHits.store(1, std::memory_order_relaxed);
            slot.firstPresent.store(present, std::memory_order_relaxed);
            slot.lastPresent.store(present, std::memory_order_relaxed);
            return;
        }
    }
    g_seqCensusOverflow.fetch_add(1, std::memory_order_relaxed);
}

// Present thread.  Window report every 300 presents while armed; release
// diff 90 presents after a manual stereo-off.
std::uint32_t SetCharWatchAllThreads(std::uintptr_t address, bool enable)
{
    if (enable)
        g_charWatchAddress.store(address, std::memory_order_release);
    g_charWatchArmed.store(false, std::memory_order_release);
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();
    std::uint32_t applied = 0;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            const auto thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            DebugRegisterUpdates::Guard registerUpdate;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context))
                {
                    context.Dr7 &= ~(0xFull << 28); // RW3/LEN3
                    context.Dr7 &= ~0xC0ull;         // L3/G3
                    if (enable)
                    {
                        context.Dr3 = address;
                        context.Dr7 |= (0xDull << 28); // RW=01 write, LEN=11 four bytes
                        context.Dr7 |= 0x40ull;        // L3
                    }
                    else
                        context.Dr3 = 0;
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context))
                        ++applied;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    g_charWatchArmed.store(enable && applied, std::memory_order_release);
    Log("[CHARWATCH] write watch %s on %u threads (address=%llX)", enable ? "ARMED" : "disarmed",
        applied, static_cast<unsigned long long>(address));
    return applied;
}

DWORD WINAPI CharWatchArmThread(LPVOID parameter)
{
    SetCharWatchAllThreads(reinterpret_cast<std::uintptr_t>(parameter), true);
    return 0;
}

DWORD WINAPI CharWatchDisarmThread(LPVOID)
{
    SetCharWatchAllThreads(g_charWatchAddress.load(std::memory_order_acquire), false);
    return 0;
}

// Exception handler side: unwind from the trap context, never touching the
// handler's own stack, and store into a fixed buffer the present thread drains.
void CharWatchRecord(const CONTEXT* trap)
{
    g_charWatchHits.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t index = g_charWatchWritten.load(std::memory_order_relaxed);
    do
    {
        if (index >= kCharWatchEvents)
        {
            g_charWatchDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    } while (!g_charWatchWritten.compare_exchange_weak(index, index + 1,
        std::memory_order_acq_rel, std::memory_order_relaxed));
    auto& event = g_charWatchEvents[index];
    event.present = g_presentCount.load(std::memory_order_relaxed);
    event.thread = GetCurrentThreadId();
    event.value = 0;
    event.frames = 0;
    const auto address = g_charWatchAddress.load(std::memory_order_relaxed);
    __try
    {
        if (address)
            event.value = *reinterpret_cast<const std::uint32_t*>(address);
        CONTEXT walk = *trap;
        for (std::uint32_t frame = 0; frame < kCharWatchFrames; ++frame)
        {
            const auto rip = walk.Rip;
            if (!rip)
                break;
            event.stack[event.frames++] = rip;
            DWORD64 imageBase = 0;
            auto* function = RtlLookupFunctionEntry(rip, &imageBase, nullptr);
            if (!function)
            {
                if (!IsReadable(reinterpret_cast<const void*>(walk.Rsp), 8))
                    break;
                walk.Rip = *reinterpret_cast<const DWORD64*>(walk.Rsp);
                walk.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, rip, function, &walk,
                &handlerData, &establisher, nullptr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void CharWatchDrain(std::uint64_t release)
{
    const auto written = g_charWatchWritten.load(std::memory_order_acquire);
    auto logged = g_charWatchLogged.load(std::memory_order_relaxed);
    while (logged < written)
    {
        const auto& event = g_charWatchEvents[logged];
        char line[1400];
        int n = std::snprintf(line, sizeof(line),
            "[CHARWATCH] write #%u present=%llu (%s%+lld) value=%08llX tid=%u stack:", logged + 1,
            static_cast<unsigned long long>(event.present), release ? "release" : "armed",
            release ? static_cast<long long>(event.present) - static_cast<long long>(release) : 0LL,
            static_cast<unsigned long long>(event.value), event.thread);
        for (std::uint32_t f = 0; f < event.frames && n < static_cast<int>(sizeof(line)) - 40; ++f)
        {
            const auto rip = event.stack[f];
            if (rip >= g_exeBase && rip < g_exeBase + kRetailImageSize)
                n += std::snprintf(line + n, sizeof(line) - n, " +%llX",
                    static_cast<unsigned long long>(rip - g_exeBase));
            else
                n += std::snprintf(line + n, sizeof(line) - n, " %llX",
                    static_cast<unsigned long long>(rip));
        }
        Log("%s", line);
        ++logged;
    }
    g_charWatchLogged.store(logged, std::memory_order_relaxed);
}

bool CharSnapshot(std::uintptr_t target, std::uint8_t* out)
{
    __try
    {
        if (!IsReadable(reinterpret_cast<const void*>(target), kCharSnapBytes))
            return false;
        std::memcpy(out, reinterpret_cast<const void*>(target), kCharSnapBytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Present thread, every present while the census is armed or reporting.
void CharDiffTick(std::uint64_t present, std::uint64_t release, const char* reportTag)
{
    const auto target = g_charTarget.load(std::memory_order_acquire);
    if (!target)
        return;
    if (target != g_charTracked)
    {
        g_charTracked = target;
        g_charHavePrev = false;
        g_charHaveRelease = false;
        g_charSamples = 0;
        std::memset(g_charVolatile, 0, sizeof(g_charVolatile));
    }
    static std::uint8_t current[kCharSnapBytes];
    const bool sampleNow = reportTag || (!release && (present % 10) == 0);
    if (!sampleNow)
        return;
    if (!CharSnapshot(target, current))
        return;
    if (!release)
    {
        if (g_charHavePrev)
        {
            for (std::size_t d = 2; d < kCharDwords; ++d)
                if (std::memcmp(current + d * 4, g_charPrev + d * 4, 4) != 0)
                    g_charVolatile[d] = true;
        }
        std::memcpy(g_charPrev, current, kCharSnapBytes);
        g_charHavePrev = true;
        ++g_charSamples;
        return;
    }
    if (!g_charHaveRelease)
    {
        // first snapshot at/after the release: fold in the last hang sample
        if (g_charHavePrev)
            for (std::size_t d = 2; d < kCharDwords; ++d)
                if (std::memcmp(current + d * 4, g_charPrev + d * 4, 4) != 0)
                    g_charVolatile[d] = true;
        std::memcpy(g_charRelease, current, kCharSnapBytes);
        g_charHaveRelease = true;
        std::uint32_t volatileCount = 0;
        for (bool v : g_charVolatile)
            volatileCount += v ? 1 : 0;
        Log("[CHARDIFF] release snapshot of %llX: %u hang samples, %u of %zu dwords volatile during the hang",
            static_cast<unsigned long long>(target), g_charSamples, volatileCount, kCharDwords);
        return;
    }
    if (!reportTag)
        return;
    char line[2000];
    int n = std::snprintf(line, sizeof(line), "[CHARDIFF] %s stable-then-changed:", reportTag);
    std::uint32_t changed = 0;
    for (std::size_t d = 2; d < kCharDwords; ++d)
    {
        if (g_charVolatile[d] || std::memcmp(current + d * 4, g_charRelease + d * 4, 4) == 0)
            continue;
        ++changed;
        if (n < static_cast<int>(sizeof(line)) - 48)
        {
            std::uint32_t a, b;
            std::memcpy(&a, g_charRelease + d * 4, 4);
            std::memcpy(&b, current + d * 4, 4);
            n += std::snprintf(line + n, sizeof(line) - n, " +%zX:%X>%X", d * 4, a, b);
        }
    }
    Log("%s  [%u dwords]", line, changed);
}

void SeqCensusTick(std::uint64_t present)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    CharDiffTick(present, g_seqCensusReleasePresent.load(std::memory_order_relaxed), nullptr);
    CharWatchDrain(g_seqCensusReleasePresent.load(std::memory_order_relaxed));
    if (!g_seqCensusArmed.load(std::memory_order_acquire) &&
        !g_seqCensusReleasePresent.load(std::memory_order_relaxed))
        return;
    static std::uint64_t lastWindow = 0;
    if (present - lastWindow >= 300)
    {
        lastWindow = present;
        char line[1400];
        int n = std::snprintf(line, sizeof(line), "[SEQCENSUS] window hits=%llu overflow=%llu:",
            static_cast<unsigned long long>(g_seqCensusHits.exchange(0, std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_seqCensusOverflow.load(std::memory_order_relaxed)));
        std::uint32_t listed = 0;
        for (auto& slot : g_seqCensus)
        {
            const auto rva = slot.vtableRva.load(std::memory_order_acquire);
            if (!rva)
                break;
            const auto hits = slot.windowHits.exchange(0, std::memory_order_relaxed);
            if (!hits || n >= static_cast<int>(sizeof(line)) - 40)
                continue;
            n += std::snprintf(line + n, sizeof(line) - n, " +%llX:%llu",
                static_cast<unsigned long long>(rva), static_cast<unsigned long long>(hits));
            ++listed;
        }
        if (listed)
            Log("%s", line);
        for (auto& slot : g_seqCensus)
        {
            const auto rva = slot.vtableRva.load(std::memory_order_acquire);
            if (!rva)
                break;
            if (slot.named.exchange(true, std::memory_order_acq_rel))
                continue;
            char name[160];
            if (SeqCensusClassName(rva, name, sizeof(name)))
                Log("[SEQCENSUS] class +%llX = %s (first=%llu)",
                    static_cast<unsigned long long>(rva), name,
                    static_cast<unsigned long long>(slot.firstPresent.load(std::memory_order_relaxed)));
            else
                Log("[SEQCENSUS] class +%llX = (no RTTI) (first=%llu)",
                    static_cast<unsigned long long>(rva),
                    static_cast<unsigned long long>(slot.firstPresent.load(std::memory_order_relaxed)));
        }
    }
    // Release report at +30, +90, +180 and +300 presents: every class that
    // was executing when stereo dropped, with its last present relative to
    // the release and its hits since.  A waiter that completes shows as a
    // small positive last=+N with hits that stop growing; whatever it
    // unblocks shows up as a class first seen after the release.
    const auto release = g_seqCensusReleasePresent.load(std::memory_order_relaxed);
    if (!release && g_seqCensusArmed.load(std::memory_order_relaxed))
    {
        static std::uint64_t lastTl = 0;
        if (present - lastTl >= 150)
        {
            lastTl = present;
            TimelineReport("tick", present, 0);
            AiWaitReport("tick", present, 0);
            SeqActivityReport(present, 150);
            ActorDiffTick("tick", present, 0);
        }
    }
    if (release)
    {
        static const std::uint64_t kTlStages[] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 50, 60, 90, 120,
            150, 180, 210, 240, 270, 300};
        static std::uint64_t tlRelease = 0;
        static std::uint32_t tlStage = 0;
        if (tlRelease != release)
        {
            tlRelease = release;
            tlStage = 0;
        }
        while (tlStage < sizeof(kTlStages) / sizeof(kTlStages[0]) &&
            present - release >= kTlStages[tlStage])
        {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "release+%llu",
                static_cast<unsigned long long>(kTlStages[tlStage]));
            TimelineReport(tag, present, release);
            AiWaitReport(tag, present, release);
            SeqActivityReport(present, 60);
            ActorDiffTick(tag, present, release);
            CharDiffTick(present, release, tag);
            if (kTlStages[tlStage] == 40 || kTlStages[tlStage] == 90)
            {
                TimelineReleaseDiff(present, release);
                AiWaitReleaseDiff(present, release);
            }
            ++tlStage;
        }
        static const std::uint64_t kStages[] = {30, 90, 180, 300};
        const auto stage = g_seqCensusReleaseStage.load(std::memory_order_relaxed);
        if (stage < 4 && present - release >= kStages[stage])
        {
            g_seqCensusReleaseStage.store(stage + 1, std::memory_order_relaxed);
            Log("[SEQCENSUS] RELEASE+%llu report (release present=%llu, instance overflow=%llu):",
                static_cast<unsigned long long>(present - release),
                static_cast<unsigned long long>(release),
                static_cast<unsigned long long>(g_seqInstOverflow.load(std::memory_order_relaxed)));
            std::uint32_t stoppedLines = 0;
            std::uint32_t newLines = 0;
            std::uint32_t stillRunning = 0;
            for (auto& slot : g_seqInst)
            {
                const auto self = slot.self.load(std::memory_order_acquire);
                if (!self)
                    continue;
                const bool before = slot.activeAtRelease.load(std::memory_order_relaxed);
                const auto last = slot.lastPresent.load(std::memory_order_relaxed);
                const auto first = slot.firstPresent.load(std::memory_order_relaxed);
                const auto since = slot.sinceRelease.load(std::memory_order_relaxed);
                const auto rva = slot.vtableRva.load(std::memory_order_relaxed);
                const bool stopped = last + 10 < present;
                char name[160];
                if (before && stopped)
                {
                    if (stoppedLines++ < 60)
                    {
                        SeqCensusClassName(rva, name, sizeof(name));
                        Log("[SEQCENSUS]   STOPPED inst=%llX last=%+lld since=%llu first=%llu "
                            "hits=%llu %s",
                            static_cast<unsigned long long>(self),
                            static_cast<long long>(last) - static_cast<long long>(release),
                            static_cast<unsigned long long>(since),
                            static_cast<unsigned long long>(first),
                            static_cast<unsigned long long>(slot.hits.load(std::memory_order_relaxed)),
                            name[0] ? name : "?");
                    }
                }
                else if (before)
                    ++stillRunning;
                else if (first >= release)
                {
                    if (newLines++ < 60)
                    {
                        SeqCensusClassName(rva, name, sizeof(name));
                        Log("[SEQCENSUS]   NEW     inst=%llX first=%+lld last=%+lld since=%llu %s",
                            static_cast<unsigned long long>(self),
                            static_cast<long long>(first) - static_cast<long long>(release),
                            static_cast<long long>(last) - static_cast<long long>(release),
                            static_cast<unsigned long long>(since),
                            name[0] ? name : "?");
                    }
                }
            }
            Log("[SEQCENSUS]   summary: stopped=%u new=%u stillRunning=%u",
                stoppedLines, newLines, stillRunning);
            if (stage + 1 == 4)
            {
                Log("[SEQCENSUS] release diff complete");
                Log("[CHARWATCH] %llu writes after the release (%llu not stored)",
                    static_cast<unsigned long long>(g_charWatchHits.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_charWatchDropped.load(std::memory_order_relaxed)));
                if (g_charWatchArmed.load(std::memory_order_acquire))
                    if (const auto worker = CreateThread(nullptr, 0, &CharWatchDisarmThread,
                            nullptr, 0, nullptr))
                        CloseHandle(worker);
                SetSequenceCensusAllThreads(false);
                g_seqCensusReleasePresent.store(0, std::memory_order_release);
            }
        }
    }
}

void SeqCensusMarkRelease(std::uint64_t present)
{
    if (!g_seqCensusArmed.load(std::memory_order_acquire))
        return;
    std::uint32_t active = 0;
    for (auto& slot : g_seqCensus)
    {
        const auto rva = slot.vtableRva.load(std::memory_order_acquire);
        if (!rva)
            break;
        const auto last = slot.lastPresent.load(std::memory_order_relaxed);
        const bool recent = last != 0 && present - last <= 60;
        slot.activeAtRelease.store(recent, std::memory_order_relaxed);
        slot.sinceRelease.store(0, std::memory_order_relaxed);
        if (recent)
            ++active;
    }
    std::uint32_t activeInst = 0;
    for (auto& slot : g_seqInst)
    {
        if (!slot.self.load(std::memory_order_acquire))
            continue;
        const auto last = slot.lastPresent.load(std::memory_order_relaxed);
        const bool recent = last != 0 && present - last <= 60;
        slot.activeAtRelease.store(recent, std::memory_order_relaxed);
        slot.sinceRelease.store(0, std::memory_order_relaxed);
        if (recent)
            ++activeInst;
    }
    Log("[SEQCENSUS] %u action instances active at the release", activeInst);
    TimelineMarkRelease();
    AiWaitMarkRelease();
    g_actorSnapHaveRelease = false;
    g_charHaveRelease = false;
    {
        // log whatever the hang produced, then give post-release writes a fresh buffer
        CharWatchDrain(0);
        Log("[CHARWATCH] %llu writes to character+0x%zX before the release (%llu not stored)",
            static_cast<unsigned long long>(g_charWatchHits.exchange(0, std::memory_order_relaxed)),
            kCharWatchOffset,
            static_cast<unsigned long long>(g_charWatchDropped.exchange(0, std::memory_order_relaxed)));
        g_charWatchWritten.store(0, std::memory_order_release);
        g_charWatchLogged.store(0, std::memory_order_relaxed);
    }
    g_seqCensusReleaseStage.store(0, std::memory_order_release);
    g_seqCensusReleaseReported.store(false, std::memory_order_release);
    g_seqCensusReleasePresent.store(present, std::memory_order_release);
    Log("[SEQCENSUS] stereo-off at present=%llu: %u action classes executed in the last 60 "
        "presents; diff reports in 90 presents",
        static_cast<unsigned long long>(present), active);
}

// [MOTIONGATE] Measured: character+0x7E4 (the motion id) was written
// ZERO times during the hang and once, two presents after stereo-off, by
// +0x5C163E0 (ActorCharacter motion setter) <- +0xE054B0 (request apply)
// <- +0xE04E50, the motion-request processor.  Before it applies a request
// that processor has two early exits, both "return 0 = try again":
//   gate A +0xE04F13: al = thunk +0x1D3420(actor->vcall+0xAB0()); al != 0 -> give up
//   gate B +0xE04F52: al = thunk +0x11E770(resource, &handle);   al == 0 -> give up
// DR2 breaks at +0xE04F25 (just after gate A returns) and DR3 at +0xE04F57
// (just after gate B returns); al is the gate's answer, r12 the request.
// Which gate keeps answering "no" for the same request during the hang,
// and flips after stereo-off, is the thing two outputs break.
constexpr std::uintptr_t kMotionGateARva = 0xE04F25;
constexpr std::uintptr_t kMotionGateBRva = 0xE04F57;
constexpr std::size_t kMotionReqSlots = 64;
struct MotionReqSlot
{
    std::atomic_uintptr_t request{};
    std::atomic_uint64_t hits[2][2]{}; // [gate][al != 0]
    std::atomic_uint64_t firstPresent{};
    std::atomic_uint64_t lastPresent{};
    std::atomic_uint64_t lastAl{};
    std::atomic_uint64_t handleQ0{};
    std::atomic_uint32_t explicitId8{};
    std::atomic_uint32_t resourceC{};
};
MotionReqSlot g_motionReqs[kMotionReqSlots]{};
std::atomic_uint64_t g_motionGateHits[2][2]{};
std::atomic_uint64_t g_motionReqOverflow{};
std::atomic_bool g_motionGateArmed{};
std::atomic_uint64_t g_motionGateArmPresent{};
std::atomic_uint64_t g_motionGateOffPresent{};

// [AIGRAPH] Static analysis: the sit request is issued by
// the AI behaviour-graph leaf BodyLeafNodeRequestAnimation (execute
// +0x8AE5680, vtable slot +0x158) and the gate probe showed the request
// processor was never reached during the hang - the graph never executes
// that leaf while two outputs render.  The walker: FUN_14776a750 iterates
// the actor's graphs -> FUN_14776cc50 steps nodes -> FUN_1477fc5c0 picks a
// transition by evaluating each transition's condition through
// FUN_147789c50(transition, ctx) -> FUN_14778a440(pin, &result, ctx, flag);
// FUN_147768770(ctx, node) executes a node (vcall +0x158).
//   DR2 exec at +0x77FC72B: right after the condition call; rbx = transition
//        entry ({transition, target, isSubgraph}), al = condition result.
//   DR3 exec at +0x7768790: inside the node executor; rbx = node.
// Per-instance census (pointer keyed): a node executed every tick through
// the hang that stops after stereo-off is the stuck node; a transition
// evaluated every tick as false that turns true after stereo-off is the
// blocked condition, and its class names what it checks.
std::uint32_t SetExecBreakpointAllThreads(int index, std::uintptr_t address, bool enable, bool onlyIfAvailable = false);
constexpr std::uintptr_t kAiGraphCondRva = 0x77FC72B;
constexpr std::uintptr_t kAiGraphNodeRva = 0x7768790;
// v2: the AI-mode trays exist from actor init; their FSM only
// ENTERS the PlayMotion state at off+2, so a filter for transitions that are
// never true before the release misses it.  v2 keys by the
// transition OBJECT, captures the owning graph data at the condition entry
// (+0x7789C50: rcx = transition, rdx = ctx, [rdx+8] = graph data with the
// pin table at +0x78/+0x80), and after stereo-off reports every transition
// evaluated through the hang whose true-count rose, with its pin bytes so
// the expression can be decoded offline.
constexpr std::uintptr_t kAiGraphCondEntryRva = 0x7789C50;
constexpr std::size_t kAiSlotsCount = 8192;
constexpr std::uint64_t kAiStalePresents = 600;
struct AiGraphSlot
{
    std::atomic_uintptr_t key{};        // node, or transition entry
    std::atomic_uintptr_t object{};     // node, or transition object (*entry)
    std::atomic_uintptr_t target{};     // transition: target node
    std::atomic_uint64_t hits{};
    std::atomic_uint64_t trueHits{};
    std::atomic_uint64_t sinceOff{};
    std::atomic_uint64_t trueSinceOff{};
    std::atomic_uint64_t firstPresent{};
    std::atomic_uint64_t lastPresent{};
    std::atomic_uint64_t lastTruePresent{};
    std::atomic_uintptr_t graphData{};
    std::atomic_uint32_t condIndex{};
};
AiGraphSlot g_aiNodes[kAiSlotsCount]{};
AiGraphSlot g_aiConds[kAiSlotsCount]{};
std::atomic_uint64_t g_aiOverflow{};
std::atomic_uint64_t g_aiHits[2]{};
std::atomic_bool g_aiGraphArmed{};
std::atomic_uint64_t g_aiGraphOffPresent{};

void AiGraphRecord(AiGraphSlot* table, std::uintptr_t key, std::uintptr_t object,
    std::uintptr_t target, bool truth, bool countTruth)
{
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const bool off = g_aiGraphOffPresent.load(std::memory_order_relaxed) != 0;
    std::size_t index = static_cast<std::size_t>((key >> 3) * 0x9E3779B97F4A7C15ull >> 51) &
        (kAiSlotsCount - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kAiSlotsCount - 1))
    {
        auto& slot = table[index];
        auto existing = slot.key.load(std::memory_order_acquire);
        if (existing == key)
        {
            slot.hits.fetch_add(1, std::memory_order_relaxed);
            slot.lastPresent.store(present, std::memory_order_relaxed);
            if (off)
                slot.sinceOff.fetch_add(1, std::memory_order_relaxed);
            if (countTruth && truth)
            {
                slot.trueHits.fetch_add(1, std::memory_order_relaxed);
                slot.lastTruePresent.store(present, std::memory_order_relaxed);
                if (off)
                    slot.trueSinceOff.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        const bool stale = existing && !off &&
            present - slot.lastPresent.load(std::memory_order_relaxed) > kAiStalePresents;
        if ((!existing || stale) && slot.key.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            slot.object.store(object, std::memory_order_relaxed);
            slot.target.store(target, std::memory_order_relaxed);
            slot.hits.store(1, std::memory_order_relaxed);
            slot.trueHits.store(countTruth && truth ? 1 : 0, std::memory_order_relaxed);
            slot.sinceOff.store(off ? 1 : 0, std::memory_order_relaxed);
            slot.trueSinceOff.store(off && countTruth && truth ? 1 : 0, std::memory_order_relaxed);
            slot.firstPresent.store(present, std::memory_order_relaxed);
            slot.lastPresent.store(present, std::memory_order_relaxed);
            slot.lastTruePresent.store(countTruth && truth ? present : 0, std::memory_order_relaxed);
            return;
        }
    }
    g_aiOverflow.fetch_add(1, std::memory_order_relaxed);
}

void AiGraphRecordCond(const CONTEXT* trap)
{
    g_aiHits[0].fetch_add(1, std::memory_order_relaxed);
    const auto entry = static_cast<std::uintptr_t>(trap->Rbx);
    std::uintptr_t object = 0, target = 0;
    __try
    {
        if (entry && IsReadable(reinterpret_cast<const void*>(entry), 16))
        {
            object = *reinterpret_cast<const std::uintptr_t*>(entry);
            target = *reinterpret_cast<const std::uintptr_t*>(entry + 8);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (object)
        AiGraphRecord(g_aiConds, object, object, target, (trap->Rax & 0xFF) != 0, true);
}

// Condition entry: rcx = transition object, rdx = ctx, [rdx+8] = graph data.
void AiGraphRecordCondEntry(const CONTEXT* trap)
{
    g_aiHits[1].fetch_add(1, std::memory_order_relaxed);
    const auto object = static_cast<std::uintptr_t>(trap->Rcx);
    const auto ctx = static_cast<std::uintptr_t>(trap->Rdx);
    std::uintptr_t graphData = 0;
    std::uint32_t index = 0xFFFFFFFFu;
    __try
    {
        if (ctx && IsReadable(reinterpret_cast<const void*>(ctx + 8), 8))
            graphData = *reinterpret_cast<const std::uintptr_t*>(ctx + 8);
        if (object && IsReadable(reinterpret_cast<const void*>(object + 0x18), 4))
            index = *reinterpret_cast<const std::uint32_t*>(object + 0x18);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (!object)
        return;
    std::size_t slotIndex = static_cast<std::size_t>((object >> 3) * 0x9E3779B97F4A7C15ull >> 51) &
        (kAiSlotsCount - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, slotIndex = (slotIndex + 1) & (kAiSlotsCount - 1))
    {
        auto& slot = g_aiConds[slotIndex];
        const auto existing = slot.key.load(std::memory_order_acquire);
        if (existing == object)
        {
            if (graphData)
                slot.graphData.store(graphData, std::memory_order_relaxed);
            slot.condIndex.store(index, std::memory_order_relaxed);
            return;
        }
        if (!existing)
            return; // the result site will create it; next evaluation fills this in
    }
}

void AiGraphRecordNode(const CONTEXT* trap)
{
    g_aiHits[1].fetch_add(1, std::memory_order_relaxed);
    const auto node = static_cast<std::uintptr_t>(trap->Rbx);
    AiGraphRecord(g_aiNodes, node, node, 0, false, false);
}

void SetAiGraphProbe(bool enable)
{
    if (!g_exeBase)
        return;
    const auto a = SetExecBreakpointAllThreads(2, g_exeBase + kAiGraphCondRva, enable);
    const auto b = SetExecBreakpointAllThreads(3, g_exeBase + kAiGraphCondEntryRva, enable);
    g_aiGraphArmed.store(enable && a && b, std::memory_order_release);
    Log("[AIGRAPH] probe %s: condition results +0x%llX on %u threads, condition entries +0x%llX on %u threads",
        enable ? "ARMED" : "disarmed", static_cast<unsigned long long>(kAiGraphCondRva), a,
        static_cast<unsigned long long>(kAiGraphCondEntryRva), b);
}
DWORD WINAPI AiGraphArmThread(LPVOID) { SetAiGraphProbe(true); return 0; }
DWORD WINAPI AiGraphDisarmThread(LPVOID) { SetAiGraphProbe(false); return 0; }

void AiGraphClassName(std::uintptr_t object, char* out, std::size_t size)
{
    out[0] = 0;
    std::uintptr_t vtable = 0;
    __try
    {
        if (object && IsReadable(reinterpret_cast<const void*>(object), 8))
            vtable = *reinterpret_cast<const std::uintptr_t*>(object);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vtable = 0;
    }
    if (vtable >= g_exeBase && vtable < g_exeBase + kRetailImageSize)
        SeqCensusClassName(vtable - g_exeBase, out, size);
    if (!out[0])
        std::snprintf(out, size, "(vt +%llX)",
            static_cast<unsigned long long>(vtable >= g_exeBase ? vtable - g_exeBase : 0));
}

// Present thread.  Every 150 presents while armed; after stereo-off at
// +10/+60/+150/+300 with the diff; then disarm.
void AiGraphTick()
{
    if (!g_aiGraphArmed.load(std::memory_order_acquire))
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto off = g_aiGraphOffPresent.load(std::memory_order_relaxed);
    static std::uint64_t lastReport = 0;
    const bool stage = off && (present - off == 10 || present - off == 60 ||
        present - off == 150 || present - off == 300);
    if (present - lastReport < 150 && !stage)
        return;
    lastReport = present;
    Log("[AIGRAPH] %s present=%llu%s%lld conditionEvals=%llu nodeExecs=%llu overflow=%llu",
        off ? "after-off" : "stereo", static_cast<unsigned long long>(present), off ? " off" : "",
        off ? static_cast<long long>(present) - static_cast<long long>(off) : 0LL,
        static_cast<unsigned long long>(g_aiHits[0].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_aiHits[1].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_aiOverflow.load(std::memory_order_relaxed)));
    if (!off)
    {
        // hang view: instances executed/evaluated on 90%+ of the last 150 presents
        std::uint32_t listed = 0;
        for (auto& slot : g_aiNodes)
        {
            const auto key = slot.key.load(std::memory_order_acquire);
            if (!key || present - slot.lastPresent.load(std::memory_order_relaxed) > 5)
                continue;
            const auto hits = slot.hits.load(std::memory_order_relaxed);
            const auto first = slot.firstPresent.load(std::memory_order_relaxed);
            if (present - first < 140 || hits < (present - first) * 9 / 10)
                continue;
            if (listed++ >= 40)
                break;
            char name[160];
            AiGraphClassName(key, name, sizeof(name));
            Log("[AIGRAPH]   node every-tick %llX hits=%llu first=%llu %s",
                static_cast<unsigned long long>(key), static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(first), name);
        }
        listed = 0;
        for (auto& slot : g_aiConds)
        {
            const auto key = slot.key.load(std::memory_order_acquire);
            if (!key || present - slot.lastPresent.load(std::memory_order_relaxed) > 5)
                continue;
            const auto hits = slot.hits.load(std::memory_order_relaxed);
            const auto first = slot.firstPresent.load(std::memory_order_relaxed);
            if (present - first < 140 || hits < (present - first) * 9 / 10)
                continue;
            if (listed++ >= 40)
                break;
            char name[160], tname[160];
            AiGraphClassName(slot.object.load(std::memory_order_relaxed), name, sizeof(name));
            AiGraphClassName(slot.target.load(std::memory_order_relaxed), tname, sizeof(tname));
            Log("[AIGRAPH]   cond every-tick entry=%llX hits=%llu true=%llu first=%llu %s -> %s",
                static_cast<unsigned long long>(key), static_cast<unsigned long long>(hits),
                static_cast<unsigned long long>(slot.trueHits.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(first), name, tname);
        }
        return;
    }
    // release view
    std::uint32_t listed = 0;
    for (auto& slot : g_aiNodes)
    {
        const auto key = slot.key.load(std::memory_order_acquire);
        if (!key)
            continue;
        const auto hits = slot.hits.load(std::memory_order_relaxed);
        const auto since = slot.sinceOff.load(std::memory_order_relaxed);
        const auto first = slot.firstPresent.load(std::memory_order_relaxed);
        const auto last = slot.lastPresent.load(std::memory_order_relaxed);
        const bool everyTickBefore = first < off && (hits - since) >= (off - first) * 9 / 10 && off - first >= 100;
        const bool newAfter = first >= off;
        const bool stoppedAfter = last + 10 < present;
        if (!(everyTickBefore && stoppedAfter) && !newAfter)
            continue;
        if (listed++ >= 60)
            break;
        char name[160];
        AiGraphClassName(key, name, sizeof(name));
        Log("[AIGRAPH]   node %s %llX first=%+lld last=%+lld hits=%llu sinceOff=%llu %s",
            newAfter ? "NEW-after-off" : "STOPPED-after-off", static_cast<unsigned long long>(key),
            static_cast<long long>(first) - static_cast<long long>(off),
            static_cast<long long>(last) - static_cast<long long>(off),
            static_cast<unsigned long long>(hits), static_cast<unsigned long long>(since), name);
    }
    listed = 0;
    for (auto& slot : g_aiConds)
    {
        const auto key = slot.key.load(std::memory_order_acquire);
        if (!key)
            continue;
        const auto hits = slot.hits.load(std::memory_order_relaxed);
        const auto since = slot.sinceOff.load(std::memory_order_relaxed);
        const auto trueHits = slot.trueHits.load(std::memory_order_relaxed);
        const auto trueSince = slot.trueSinceOff.load(std::memory_order_relaxed);
        const auto first = slot.firstPresent.load(std::memory_order_relaxed);
        const bool evaluatedThroughHang = first < off && (hits - since) >= (off - first) * 9 / 10 && off - first >= 100;
        const bool trueAfter = trueSince > 0;
        const bool newAfter = first >= off;
        if (!(evaluatedThroughHang && trueAfter) && !(newAfter && trueAfter))
            continue;
        if (listed++ >= 60)
            break;
        char name[160], tname[160];
        AiGraphClassName(slot.object.load(std::memory_order_relaxed), name, sizeof(name));
        AiGraphClassName(slot.target.load(std::memory_order_relaxed), tname, sizeof(tname));
        const auto graphData = slot.graphData.load(std::memory_order_relaxed);
        const auto condIndex = slot.condIndex.load(std::memory_order_relaxed);
        Log("[AIGRAPH]   cond %s obj=%llX first=%+lld hitsBefore=%llu trueBefore=%llu "
            "trueAfter=%llu firstTrue=%+lld idx=%u graph=%llX %s -> %s",
            newAfter ? "NEW-after-off" : "TRUE-after-off", static_cast<unsigned long long>(key),
            static_cast<long long>(first) - static_cast<long long>(off),
            static_cast<unsigned long long>(hits - since), static_cast<unsigned long long>(trueHits - trueSince),
            static_cast<unsigned long long>(trueSince),
            static_cast<long long>(slot.lastTruePresent.load(std::memory_order_relaxed)) - static_cast<long long>(off),
            condIndex, static_cast<unsigned long long>(graphData), name, tname);
        // pin dump: graphData+0x78 = pin table, +0x80 = count; pin+0x20 = expression
        __try
        {
            if (graphData && condIndex != 0xFFFFFFFFu &&
                IsReadable(reinterpret_cast<const void*>(graphData + 0x78), 16))
            {
                const auto table = *reinterpret_cast<const std::uintptr_t*>(graphData + 0x78);
                const auto count = *reinterpret_cast<const std::uint32_t*>(graphData + 0x80);
                if (table && condIndex < count &&
                    IsReadable(reinterpret_cast<const void*>(table + condIndex * 8), 8))
                {
                    const auto pin = *reinterpret_cast<const std::uintptr_t*>(table + condIndex * 8);
                    char hex[400];
                    int n = std::snprintf(hex, sizeof(hex), "[AIGRAPH]     pin=%llX bytes:",
                        static_cast<unsigned long long>(pin));
                    if (pin && IsReadable(reinterpret_cast<const void*>(pin), 0x40))
                    {
                        for (std::size_t b = 0; b < 0x40; ++b)
                            n += std::snprintf(hex + n, sizeof(hex) - n, "%s%02X", (b % 8) ? "" : " ",
                                reinterpret_cast<const std::uint8_t*>(pin)[b]);
                        Log("%s", hex);
                        const auto expr = *reinterpret_cast<const std::uintptr_t*>(pin + 0x20);
                        if (expr && IsReadable(reinterpret_cast<const void*>(expr), 0x60))
                        {
                            n = std::snprintf(hex, sizeof(hex), "[AIGRAPH]     expr=%llX bytes:",
                                static_cast<unsigned long long>(expr));
                            for (std::size_t b = 0; b < 0x60; ++b)
                                n += std::snprintf(hex + n, sizeof(hex) - n, "%s%02X", (b % 8) ? "" : " ",
                                    reinterpret_cast<const std::uint8_t*>(expr)[b]);
                            Log("%s", hex);
                        }
                    }
                    else
                        Log("%s (unreadable)", hex);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    if (present - off >= 300)
    {
        g_aiGraphArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &AiGraphDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

// [AIMODE] Measured: the character's AI-mode graph (AIGraphTrayAIModeFSM
// and its FSM roots) was constructed for the first time two presents after
// stereo-off - the sit never entered the graph while two outputs rendered.
// Ghidra: ExecAIModePlayMotion's start (+0xA155E20) hands the request to
// the actor's AI component via +0x5E3A040 -> +0x5E3C450, which stores mode
// id at comp+0x94, sub-id +0x98 and sets PENDING comp+0x9C = 1.  Something
// in the AI component's update consumes the pending request and builds the
// tray.  DR2 executes at +0x5E3C450 (rcx = AI component, edx = mode id) with
// a call stack; once the PlayMotion request (mode 0x1015A6D) is seen, DR3
// becomes a 4-byte WRITE watch on that component's +0x9C with a call stack.
// No write through the hang and a write right after stereo-off names the
// consumer and the update that is starved with two outputs.
constexpr std::uintptr_t kAiModeRequestRva = 0x5E3C450;
// A write watch on the heap field +0x9C ends the process silently (no WER
// record, no dump), so DR3 is an execute
// breakpoint on the AIGraphTrayAIModeFSM constructor (+0x89B70A0) instead,
// which runs exactly when the mode starts and captures the consumer's stack.
constexpr std::uintptr_t kAiModeTrayCtorRva = 0x89B70A0;
constexpr std::uint32_t kAiModePlayMotionId = 0x1015A6D;
constexpr std::size_t kTraceEvents = 96;
constexpr std::size_t kTraceFrames = 14;
struct TraceEvent
{
    std::uint32_t site;   // 0 = request, 1 = pending write
    std::uint32_t thread;
    std::uint64_t present;
    std::uint64_t a, b, c; // site 0: rcx, rdx, r8; site 1: value at +0x9C, rip, 0
    std::uint32_t frames;
    std::uint64_t stack[kTraceFrames];
};
TraceEvent g_trace[kTraceEvents]{};
std::atomic_uint32_t g_traceWritten{};
std::atomic_uint32_t g_traceLogged{};
std::atomic_uint64_t g_traceDropped{};
std::atomic_uint64_t g_traceHits[2]{};
std::atomic_uintptr_t g_aiModeComponent{};
std::atomic_uintptr_t g_aiModePendingAddr{};
std::atomic_bool g_aiModeArmed{};
std::atomic_bool g_aiModeWatchArmed{};
std::atomic_uint64_t g_aiModeOffPresent{};

void TraceCapture(std::uint32_t site, const CONTEXT* trap, std::uint64_t a, std::uint64_t b,
    std::uint64_t c)
{
    g_traceHits[site & 1].fetch_add(1, std::memory_order_relaxed);
    std::uint32_t index = g_traceWritten.load(std::memory_order_relaxed);
    do
    {
        if (index >= kTraceEvents)
        {
            g_traceDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    } while (!g_traceWritten.compare_exchange_weak(index, index + 1,
        std::memory_order_acq_rel, std::memory_order_relaxed));
    auto& event = g_trace[index];
    event.site = site;
    event.present = g_presentCount.load(std::memory_order_relaxed);
    event.thread = GetCurrentThreadId();
    event.a = a;
    event.b = b;
    event.c = c;
    event.frames = 0;
    __try
    {
        CONTEXT walk = *trap;
        for (std::uint32_t frame = 0; frame < kTraceFrames; ++frame)
        {
            const auto rip = walk.Rip;
            if (!rip)
                break;
            event.stack[event.frames++] = rip;
            DWORD64 imageBase = 0;
            auto* function = RtlLookupFunctionEntry(rip, &imageBase, nullptr);
            if (!function)
            {
                if (!IsReadable(reinterpret_cast<const void*>(walk.Rsp), 8))
                    break;
                walk.Rip = *reinterpret_cast<const DWORD64*>(walk.Rsp);
                walk.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, rip, function, &walk,
                &handlerData, &establisher, nullptr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

std::uint32_t SetExecBreakpointAllThreads(int index, std::uintptr_t address, bool enable, bool onlyIfAvailable);
std::uint32_t SetCharWatchAllThreads(std::uintptr_t address, bool enable);

DWORD WINAPI AiModeArmThread(LPVOID)
{
    const auto n = SetExecBreakpointAllThreads(2, g_exeBase + kAiModeRequestRva, true);
    const auto m = SetExecBreakpointAllThreads(3, g_exeBase + kAiModeTrayCtorRva, true);
    g_aiModeArmed.store(n != 0, std::memory_order_release);
    Log("[AIMODE] request breakpoint ARMED on %u threads (+0x%llX); tray ctor breakpoint on %u threads (+0x%llX)",
        n, static_cast<unsigned long long>(kAiModeRequestRva), m,
        static_cast<unsigned long long>(kAiModeTrayCtorRva));
    return 0;
}

DWORD WINAPI AiModeWatchThread(LPVOID parameter)
{
    // DR3 write watch on the AI component's pending flag (reuses the
    // CHARWATCH register programming: RW=01, LEN=11).
    const auto address = reinterpret_cast<std::uintptr_t>(parameter);
    const auto n = SetCharWatchAllThreads(address, true);
    g_aiModeWatchArmed.store(n != 0, std::memory_order_release);
    Log("[AIMODE] pending-flag write watch ARMED on %u threads (component+0x9C = %llX)", n,
        static_cast<unsigned long long>(address));
    return 0;
}

DWORD WINAPI AiModeDisarmThread(LPVOID)
{
    SetExecBreakpointAllThreads(2, 0, false);
    SetExecBreakpointAllThreads(3, 0, false);
    g_aiModeWatchArmed.store(false, std::memory_order_release);
    g_aiModeArmed.store(false, std::memory_order_release);
    Log("[AIMODE] probe disarmed");
    return 0;
}

void AiModeRecordRequest(const CONTEXT* trap)
{
    TraceCapture(0, trap, trap->Rcx, trap->Rdx & 0xFFFFFFFF, trap->R8 & 0xFFFFFFFF);
    if ((trap->Rdx & 0xFFFFFFFF) == kAiModePlayMotionId && trap->Rcx &&
        !g_aiModeComponent.load(std::memory_order_relaxed))
    {
        g_aiModeComponent.store(static_cast<std::uintptr_t>(trap->Rcx), std::memory_order_release);
        g_aiModePendingAddr.store(static_cast<std::uintptr_t>(trap->Rcx) + 0x9C,
            std::memory_order_release);
    }
}

void AiModeRecordPendingWrite(const CONTEXT* trap)
{
    std::uint32_t value = 0;
    __try
    {
        value = *reinterpret_cast<const std::uint32_t*>(
            g_aiModePendingAddr.load(std::memory_order_relaxed));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    TraceCapture(1, trap, value, trap->Rip, 0);
}

// [TRAYTICK] Measured: no AI-graph transition flipped from
// false to true - every transition evaluated through the hang was true on
// all 2096 evaluations - and the transitions that fired after stereo-off
// were first evaluated 2-6 presents after it.  The scene actors' AI-mode
// state machines are simply not ticked while two outputs render.  Their
// tick is the tray update vtable slot +0x158: AIGraphTrayAIModeFSM
// +0x89B7580 and AIGraphTrayMindTaskFSM +0x89BB630 (both -> graph update
// +0x78072B0).  DR2/DR3 execute there: per-tray hit counts before and after
// stereo-off, and call stacks for the first hits in each phase, name the
// caller that stops calling in stereo.
constexpr std::uintptr_t kTrayAiModeUpdateRva = 0x89B7580;
constexpr std::uintptr_t kTrayMindTaskUpdateRva = 0x89BB630;
constexpr std::size_t kTraySlots = 256;
struct TraySlot
{
    std::atomic_uintptr_t tray{};
    std::atomic_uint32_t kind{};
    std::atomic_uint64_t before{};
    std::atomic_uint64_t after{};
    std::atomic_uint64_t firstPresent{};
    std::atomic_uint64_t lastPresent{};
};
TraySlot g_trays[kTraySlots]{};
std::atomic_bool g_trayArmed{};
std::atomic_uint64_t g_trayOffPresent{};
std::atomic_uint64_t g_trayHits[2][2]{}; // [kind][after]
std::atomic_uint32_t g_trayStacksBefore{};
std::atomic_uint32_t g_trayStacksAfter{};

void TrayRecord(int kind, const CONTEXT* trap)
{
    const bool after = g_trayOffPresent.load(std::memory_order_relaxed) != 0;
    g_trayHits[kind][after ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    const auto tray = static_cast<std::uintptr_t>(trap->Rcx);
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    std::size_t index = static_cast<std::size_t>((tray >> 3) * 0x9E3779B97F4A7C15ull >> 56) &
        (kTraySlots - 1);
    for (std::size_t probe = 0; probe < 32; ++probe, index = (index + 1) & (kTraySlots - 1))
    {
        auto& slot = g_trays[index];
        auto existing = slot.tray.load(std::memory_order_acquire);
        if (existing != tray)
        {
            if (existing || !slot.tray.compare_exchange_strong(existing, tray,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                if (existing != tray)
                    continue;
            }
            else
            {
                slot.kind.store(static_cast<std::uint32_t>(kind), std::memory_order_relaxed);
                slot.firstPresent.store(present, std::memory_order_relaxed);
            }
        }
        (after ? slot.after : slot.before).fetch_add(1, std::memory_order_relaxed);
        slot.lastPresent.store(present, std::memory_order_relaxed);
        break;
    }
    // stacks: first 4 hits in stereo, first 10 after stereo-off
    auto& budget = after ? g_trayStacksAfter : g_trayStacksBefore;
    if (budget.load(std::memory_order_relaxed) < (after ? 10u : 4u) &&
        budget.fetch_add(1, std::memory_order_relaxed) < (after ? 10u : 4u))
        TraceCapture(static_cast<std::uint32_t>(kind), trap, tray, after ? 1 : 0, 0);
}

DWORD WINAPI TrayArmThread(LPVOID)
{
    const auto a = SetExecBreakpointAllThreads(2, g_exeBase + kTrayAiModeUpdateRva, true);
    const auto b = SetExecBreakpointAllThreads(3, g_exeBase + kTrayMindTaskUpdateRva, true);
    g_trayArmed.store(a != 0 || b != 0, std::memory_order_release);
    Log("[TRAYTICK] ARMED: AIModeFSM update +0x%llX on %u threads, MindTaskFSM update +0x%llX on %u threads",
        static_cast<unsigned long long>(kTrayAiModeUpdateRva), a,
        static_cast<unsigned long long>(kTrayMindTaskUpdateRva), b);
    return 0;
}

DWORD WINAPI TrayDisarmThread(LPVOID)
{
    SetExecBreakpointAllThreads(2, 0, false);
    SetExecBreakpointAllThreads(3, 0, false);
    Log("[TRAYTICK] disarmed");
    return 0;
}

void TrayDrain()
{
    const auto written = g_traceWritten.load(std::memory_order_acquire);
    auto logged = g_traceLogged.load(std::memory_order_relaxed);
    const auto off = g_trayOffPresent.load(std::memory_order_relaxed);
    while (logged < written)
    {
        const auto& e = g_trace[logged];
        char line[1500];
        int n = std::snprintf(line, sizeof(line),
            "[TRAYTICK] %s-update %s present=%llu tray=%llX tid=%u stack:",
            e.site == 0 ? "AIModeFSM" : "MindTaskFSM", e.b ? "AFTER-OFF" : "in-stereo",
            static_cast<unsigned long long>(e.present), static_cast<unsigned long long>(e.a), e.thread);
        for (std::uint32_t f = 0; f < e.frames && n < static_cast<int>(sizeof(line)) - 40; ++f)
        {
            const auto rip = e.stack[f];
            if (rip >= g_exeBase && rip < g_exeBase + kRetailImageSize)
                n += std::snprintf(line + n, sizeof(line) - n, " +%llX",
                    static_cast<unsigned long long>(rip - g_exeBase));
            else
                n += std::snprintf(line + n, sizeof(line) - n, " %llX",
                    static_cast<unsigned long long>(rip));
        }
        Log("%s", line);
        ++logged;
    }
    g_traceLogged.store(logged, std::memory_order_relaxed);
    (void)off;
}

void TrayTick()
{
    if (!g_trayArmed.load(std::memory_order_acquire))
        return;
    TrayDrain();
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto off = g_trayOffPresent.load(std::memory_order_relaxed);
    static std::uint64_t lastSummary = 0;
    const bool stage = off && (present - off == 10 || present - off == 60 || present - off == 300);
    if (present - lastSummary < 300 && !stage)
        return;
    lastSummary = present;
    Log("[TRAYTICK] %s present=%llu AIModeFSM updates stereo/after=%llu/%llu  MindTaskFSM updates stereo/after=%llu/%llu",
        off ? "after-off" : "stereo", static_cast<unsigned long long>(present),
        static_cast<unsigned long long>(g_trayHits[0][0].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_trayHits[0][1].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_trayHits[1][0].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_trayHits[1][1].load(std::memory_order_relaxed)));
    if (!stage)
        return;
    std::uint32_t listed = 0;
    for (auto& slot : g_trays)
    {
        const auto tray = slot.tray.load(std::memory_order_acquire);
        if (!tray)
            continue;
        const auto before = slot.before.load(std::memory_order_relaxed);
        const auto afterHits = slot.after.load(std::memory_order_relaxed);
        if (listed++ >= 80)
            break;
        Log("[TRAYTICK]   tray=%llX %s updatesInStereo=%llu updatesAfterOff=%llu first=%llu last=%+lld %s",
            static_cast<unsigned long long>(tray), slot.kind.load(std::memory_order_relaxed) ? "MindTask" : "AIMode  ",
            static_cast<unsigned long long>(before), static_cast<unsigned long long>(afterHits),
            static_cast<unsigned long long>(slot.firstPresent.load(std::memory_order_relaxed)),
            static_cast<long long>(slot.lastPresent.load(std::memory_order_relaxed)) - static_cast<long long>(off),
            before == 0 && afterHits > 0 ? "<- STARTED AFTER OFF" : "");
    }
    if (present - off >= 300)
    {
        g_trayArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &TrayDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

// [ACTUPD] Measured: AI state-machine updates for the scene
// actors all but stopped from just before the scripted camera (660 in the
// first 600 presents, one in the next 1350) and the sit's AI-mode trays
// updated for the first time one present after stereo-off.  Both phases
// share the chain job +0x2DC1E1F -> +0x5DC70C3 -> actor update +0xF3750
// (vtable +0x1048) -> AI component list +0x5EB9410 -> AI component update
// +0x5E67ED0.  The actor update has three gates in front of the component
// list (decompiled):
//   actor+0xF1 == 0 -> skip;  actor+0xF2 != 0 -> skip;
//   ctl = actor->vcall+0xAB0(); ctl && thunk +0x1D3440 (-> +0x61079F0)(ctl) != 0 -> skip
// (the same control object the motion processor's gate A queries).
// DR3 executes at +0xF37AB (the F1 test: rbx = actor, rdi = ctl) and DR2 at
// +0xF37D4 (right after the control check: eax = its answer).  Per actor:
// update calls, flag values and control answers in stereo vs after
// stereo-off; actors whose answers change across the release are listed first.
constexpr std::uintptr_t kActUpdFlagRva = 0xF37AB;
constexpr std::uintptr_t kActUpdCtlRva = 0xF37D4;
constexpr std::size_t kActSlots = 1024;
struct ActSlot
{
    std::atomic_uintptr_t actor{};
    std::atomic_uint64_t calls[2]{};
    std::atomic_uint64_t f1Zero[2]{};
    std::atomic_uint64_t f2Set[2]{};
    std::atomic_uint64_t ctlHits[2]{};
    std::atomic_uint64_t ctlSkip[2]{};
    std::atomic_uint64_t noCtl[2]{};
    std::atomic_uintptr_t ctl{};
    std::atomic_uint32_t lastCtlAnswer{};
};
ActSlot g_acts[kActSlots]{};
std::atomic_bool g_actArmed{};
std::atomic_uint64_t g_actOffPresent{};
std::atomic_uint64_t g_actOverflow{};

ActSlot* ActFind(std::uintptr_t actor)
{
    std::size_t index = static_cast<std::size_t>((actor >> 3) * 0x9E3779B97F4A7C15ull >> 54) &
        (kActSlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kActSlots - 1))
    {
        auto& slot = g_acts[index];
        auto existing = slot.actor.load(std::memory_order_acquire);
        if (existing == actor)
            return &slot;
        if (!existing && slot.actor.compare_exchange_strong(existing, actor,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return &slot;
        if (existing == actor)
            return &slot;
    }
    g_actOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void ActRecordFlags(const CONTEXT* trap)
{
    const auto actor = static_cast<std::uintptr_t>(trap->Rbx);
    if (!actor)
        return;
    auto* slot = ActFind(actor);
    if (!slot)
        return;
    const int phase = g_actOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    slot->calls[phase].fetch_add(1, std::memory_order_relaxed);
    std::uint8_t f1 = 0, f2 = 0;
    __try
    {
        f1 = *reinterpret_cast<const std::uint8_t*>(actor + 0xF1);
        f2 = *reinterpret_cast<const std::uint8_t*>(actor + 0xF2);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (!f1)
        slot->f1Zero[phase].fetch_add(1, std::memory_order_relaxed);
    if (f2)
        slot->f2Set[phase].fetch_add(1, std::memory_order_relaxed);
    if (!trap->Rdi && f1 && !f2)
        slot->noCtl[phase].fetch_add(1, std::memory_order_relaxed);
}

void ActRecordCtl(const CONTEXT* trap)
{
    const auto actor = static_cast<std::uintptr_t>(trap->Rbx);
    if (!actor)
        return;
    auto* slot = ActFind(actor);
    if (!slot)
        return;
    const int phase = g_actOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    const auto answer = static_cast<std::uint32_t>(trap->Rax);
    slot->ctlHits[phase].fetch_add(1, std::memory_order_relaxed);
    if (answer != 0)
        slot->ctlSkip[phase].fetch_add(1, std::memory_order_relaxed);
    slot->lastCtlAnswer.store(answer, std::memory_order_relaxed);
    slot->ctl.store(static_cast<std::uintptr_t>(trap->Rdi), std::memory_order_relaxed);
}

DWORD WINAPI ActArmThread(LPVOID)
{
    const auto a = SetExecBreakpointAllThreads(3, g_exeBase + kActUpdFlagRva, true);
    const auto b = SetExecBreakpointAllThreads(2, g_exeBase + kActUpdCtlRva, true);
    g_actArmed.store(a != 0 && b != 0, std::memory_order_release);
    Log("[ACTUPD] ARMED: flag test +0x%llX on %u threads, control answer +0x%llX on %u threads",
        static_cast<unsigned long long>(kActUpdFlagRva), a,
        static_cast<unsigned long long>(kActUpdCtlRva), b);
    return 0;
}

DWORD WINAPI ActDisarmThread(LPVOID)
{
    SetExecBreakpointAllThreads(2, 0, false);
    SetExecBreakpointAllThreads(3, 0, false);
    Log("[ACTUPD] disarmed");
    return 0;
}

void ActTick()
{
    if (!g_actArmed.load(std::memory_order_acquire))
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto off = g_actOffPresent.load(std::memory_order_relaxed);
    static std::uint64_t lastSummary = 0;
    const bool stage = off && (present - off == 10 || present - off == 60 || present - off == 300);
    if (present - lastSummary < 300 && !stage)
        return;
    lastSummary = present;
    std::uint64_t tot[2]{}, skip[2]{}, f2[2]{}, actors = 0;
    for (auto& s : g_acts)
    {
        if (!s.actor.load(std::memory_order_acquire))
            continue;
        ++actors;
        for (int ph = 0; ph < 2; ++ph)
        {
            tot[ph] += s.calls[ph].load(std::memory_order_relaxed);
            skip[ph] += s.ctlSkip[ph].load(std::memory_order_relaxed);
            f2[ph] += s.f2Set[ph].load(std::memory_order_relaxed);
        }
    }
    Log("[ACTUPD] %s present=%llu actors=%llu updates stereo/after=%llu/%llu  controlSkips=%llu/%llu  f2Set=%llu/%llu overflow=%llu",
        off ? "after-off" : "stereo", static_cast<unsigned long long>(present),
        static_cast<unsigned long long>(actors),
        static_cast<unsigned long long>(tot[0]), static_cast<unsigned long long>(tot[1]),
        static_cast<unsigned long long>(skip[0]), static_cast<unsigned long long>(skip[1]),
        static_cast<unsigned long long>(f2[0]), static_cast<unsigned long long>(f2[1]),
        static_cast<unsigned long long>(g_actOverflow.load(std::memory_order_relaxed)));
    if (!stage)
        return;
    // changed actors first, then a sample of the rest
    for (int pass = 0; pass < 2; ++pass)
    {
        std::uint32_t listed = 0;
        for (auto& s : g_acts)
        {
            const auto actor = s.actor.load(std::memory_order_acquire);
            if (!actor)
                continue;
            const auto c0 = s.calls[0].load(std::memory_order_relaxed);
            const auto c1 = s.calls[1].load(std::memory_order_relaxed);
            const auto k0 = s.ctlSkip[0].load(std::memory_order_relaxed);
            const auto k1 = s.ctlSkip[1].load(std::memory_order_relaxed);
            const auto h0 = s.ctlHits[0].load(std::memory_order_relaxed);
            const auto h1 = s.ctlHits[1].load(std::memory_order_relaxed);
            const auto z0 = s.f1Zero[0].load(std::memory_order_relaxed);
            const auto z1 = s.f1Zero[1].load(std::memory_order_relaxed);
            const auto g0 = s.f2Set[0].load(std::memory_order_relaxed);
            const auto g1 = s.f2Set[1].load(std::memory_order_relaxed);
            const bool skipAllBefore = h0 > 0 && k0 == h0;
            const bool skipAllAfter = h1 > 0 && k1 == h1;
            const bool f2AllBefore = c0 > 0 && g0 == c0;
            const bool f2AllAfter = c1 > 0 && g1 == c1;
            const bool f1AllBefore = c0 > 0 && z0 == c0;
            const bool f1AllAfter = c1 > 0 && z1 == c1;
            const bool changed = (c0 == 0) != (c1 == 0) || (c1 > 0 && c0 > 0 &&
                (skipAllBefore != skipAllAfter || f2AllBefore != f2AllAfter || f1AllBefore != f1AllAfter));
            if ((pass == 0) != changed)
                continue;
            if (listed++ >= (pass == 0 ? 80u : 25u))
                break;
            Log("[ACTUPD]   %s actor=%llX calls=%llu/%llu f1zero=%llu/%llu f2set=%llu/%llu ctl=%llX ctlHits=%llu/%llu ctlSkip=%llu/%llu noCtl=%llu/%llu lastAnswer=%u",
                changed ? "CHANGED" : "same   ", static_cast<unsigned long long>(actor),
                static_cast<unsigned long long>(c0), static_cast<unsigned long long>(c1),
                static_cast<unsigned long long>(z0), static_cast<unsigned long long>(z1),
                static_cast<unsigned long long>(g0), static_cast<unsigned long long>(g1),
                static_cast<unsigned long long>(s.ctl.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(h0), static_cast<unsigned long long>(h1),
                static_cast<unsigned long long>(k0), static_cast<unsigned long long>(k1),
                static_cast<unsigned long long>(s.noCtl[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.noCtl[1].load(std::memory_order_relaxed)),
                s.lastCtlAnswer.load(std::memory_order_relaxed));
        }
    }
    if (present - off >= 300)
    {
        g_actArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &ActDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

// [AICOMP] Measured: the per-actor update's three gates passed
// for every actor in stereo (49 actors, 59k updates, zero control skips),
// so the per-actor update reaches the AI components.  The AI component
// update +0x5E67ED0 then skips its four graph holders when:
//   state = actor->vcall+0x9F0()->+0xF4 is not 0 (3 -> set 4 and return);
//   actor->vcall+0x558() returns false;
// and each holder +0x11D620 skips when dt (*rdx, int) scales to 0.0 or
// holder+0x49 is set.  dt comes from +0x5DC7060: dt * timescale
// (+0x5C00250), or a per-actor clock (+0x5C00450) for actor type 0x1025F8A.
// DR2 executes at +0x5E67F2C (rax = state object, rsi = component, rdi =
// actor, rbp = &dt) and DR3 at +0x5E67F58 (al = vcall+0x558 answer); DR3
// also reads the four holders' +0x48/+0x49 flags.  Per component, stereo vs
// after stereo-off; components whose behaviour changes are listed first.
constexpr std::uintptr_t kAiCompStateRva = 0x5E67F2C;
constexpr std::uintptr_t kAiCompGateRva = 0x5E67F58;
constexpr std::size_t kCompSlots = 1024;
struct CompSlot
{
    std::atomic_uintptr_t comp{};
    std::atomic_uintptr_t actor{};
    std::atomic_uint64_t calls[2]{};
    std::atomic_uint64_t state0[2]{};
    std::atomic_uint64_t state3[2]{};
    std::atomic_uint64_t stateOther[2]{};
    std::atomic_uint32_t lastState{};
    std::atomic_uint64_t dtZero[2]{};
    std::atomic<std::int32_t> lastDt{};
    std::atomic_uint64_t gateHits[2]{};
    std::atomic_uint64_t gateFalse[2]{};
    std::atomic_uint64_t holderPaused[2]{};
    std::atomic_uint32_t lastHolderFlags{};
};
CompSlot g_comps[kCompSlots]{};
std::atomic_bool g_compArmed{};
std::atomic_uint64_t g_compOffPresent{};
std::atomic_uint64_t g_compOverflow{};

CompSlot* CompFind(std::uintptr_t comp)
{
    std::size_t index = static_cast<std::size_t>((comp >> 3) * 0x9E3779B97F4A7C15ull >> 54) &
        (kCompSlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kCompSlots - 1))
    {
        auto& slot = g_comps[index];
        auto existing = slot.comp.load(std::memory_order_acquire);
        if (existing == comp)
            return &slot;
        if (!existing && slot.comp.compare_exchange_strong(existing, comp,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return &slot;
        if (existing == comp)
            return &slot;
    }
    g_compOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void CompRecordState(const CONTEXT* trap)
{
    auto* slot = CompFind(static_cast<std::uintptr_t>(trap->Rsi));
    if (!slot)
        return;
    const int ph = g_compOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    slot->actor.store(static_cast<std::uintptr_t>(trap->Rdi), std::memory_order_relaxed);
    slot->calls[ph].fetch_add(1, std::memory_order_relaxed);
    std::uint32_t state = 0xFFFFFFFF;
    std::int32_t dt = 0;
    __try
    {
        if (trap->Rax)
            state = *reinterpret_cast<const std::uint32_t*>(trap->Rax + 0xF4);
        if (trap->Rbp)
            dt = *reinterpret_cast<const std::int32_t*>(trap->Rbp);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (state == 0)
        slot->state0[ph].fetch_add(1, std::memory_order_relaxed);
    else if (state == 3)
        slot->state3[ph].fetch_add(1, std::memory_order_relaxed);
    else
        slot->stateOther[ph].fetch_add(1, std::memory_order_relaxed);
    slot->lastState.store(state, std::memory_order_relaxed);
    if (dt == 0)
        slot->dtZero[ph].fetch_add(1, std::memory_order_relaxed);
    slot->lastDt.store(dt, std::memory_order_relaxed);
}

void CompRecordGate(const CONTEXT* trap)
{
    const auto comp = static_cast<std::uintptr_t>(trap->Rsi);
    auto* slot = CompFind(comp);
    if (!slot)
        return;
    const int ph = g_compOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    slot->gateHits[ph].fetch_add(1, std::memory_order_relaxed);
    if ((trap->Rax & 0xFF) == 0)
        slot->gateFalse[ph].fetch_add(1, std::memory_order_relaxed);
    std::uint32_t flags = 0;
    bool paused = false;
    __try
    {
        static const std::uintptr_t kHolderOffsets[4] = {0x68, 0x58, 0x60, 0x70};
        for (int i = 0; i < 4; ++i)
        {
            const auto holder = *reinterpret_cast<const std::uint64_t*>(comp + kHolderOffsets[i]);
            if (!holder)
                continue;
            const auto f48 = *reinterpret_cast<const std::uint8_t*>(holder + 0x48);
            const auto f49 = *reinterpret_cast<const std::uint8_t*>(holder + 0x49);
            flags |= (static_cast<std::uint32_t>(f48 ? 1 : 0) | (f49 ? 2u : 0u)) << (i * 2);
            if (f49)
                paused = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    if (paused)
        slot->holderPaused[ph].fetch_add(1, std::memory_order_relaxed);
    slot->lastHolderFlags.store(flags, std::memory_order_relaxed);
}

DWORD WINAPI CompArmThread(LPVOID)
{
    const auto a = SetExecBreakpointAllThreads(2, g_exeBase + kAiCompStateRva, true);
    const auto b = SetExecBreakpointAllThreads(3, g_exeBase + kAiCompGateRva, true);
    g_compArmed.store(a != 0 && b != 0, std::memory_order_release);
    Log("[AICOMP] ARMED: state read +0x%llX on %u threads, gate answer +0x%llX on %u threads",
        static_cast<unsigned long long>(kAiCompStateRva), a,
        static_cast<unsigned long long>(kAiCompGateRva), b);
    return 0;
}

DWORD WINAPI CompDisarmThread(LPVOID)
{
    SetExecBreakpointAllThreads(2, 0, false);
    SetExecBreakpointAllThreads(3, 0, false);
    Log("[AICOMP] disarmed");
    return 0;
}

void CompTick()
{
    if (!g_compArmed.load(std::memory_order_acquire))
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto off = g_compOffPresent.load(std::memory_order_relaxed);
    static std::uint64_t lastSummary = 0;
    const bool stage = off && (present - off == 10 || present - off == 60 || present - off == 300);
    if (present - lastSummary < 300 && !stage)
        return;
    lastSummary = present;
    std::uint64_t n = 0, calls[2]{}, nonzeroState[2]{}, dtz[2]{}, gf[2]{}, hp[2]{};
    for (auto& s : g_comps)
    {
        if (!s.comp.load(std::memory_order_acquire))
            continue;
        ++n;
        for (int ph = 0; ph < 2; ++ph)
        {
            calls[ph] += s.calls[ph].load(std::memory_order_relaxed);
            nonzeroState[ph] += s.state3[ph].load(std::memory_order_relaxed) +
                s.stateOther[ph].load(std::memory_order_relaxed);
            dtz[ph] += s.dtZero[ph].load(std::memory_order_relaxed);
            gf[ph] += s.gateFalse[ph].load(std::memory_order_relaxed);
            hp[ph] += s.holderPaused[ph].load(std::memory_order_relaxed);
        }
    }
    Log("[AICOMP] %s present=%llu components=%llu updates=%llu/%llu stateNonzero=%llu/%llu dtZero=%llu/%llu gateFalse=%llu/%llu holderPaused=%llu/%llu overflow=%llu",
        off ? "after-off" : "stereo", static_cast<unsigned long long>(present),
        static_cast<unsigned long long>(n),
        static_cast<unsigned long long>(calls[0]), static_cast<unsigned long long>(calls[1]),
        static_cast<unsigned long long>(nonzeroState[0]), static_cast<unsigned long long>(nonzeroState[1]),
        static_cast<unsigned long long>(dtz[0]), static_cast<unsigned long long>(dtz[1]),
        static_cast<unsigned long long>(gf[0]), static_cast<unsigned long long>(gf[1]),
        static_cast<unsigned long long>(hp[0]), static_cast<unsigned long long>(hp[1]),
        static_cast<unsigned long long>(g_compOverflow.load(std::memory_order_relaxed)));
    if (!stage)
        return;
    for (int pass = 0; pass < 2; ++pass)
    {
        std::uint32_t listed = 0;
        for (auto& s : g_comps)
        {
            const auto comp = s.comp.load(std::memory_order_acquire);
            if (!comp)
                continue;
            const auto c0 = s.calls[0].load(std::memory_order_relaxed);
            const auto c1 = s.calls[1].load(std::memory_order_relaxed);
            if (!c1)
                continue; // only components still updated after stereo-off
            const auto z0 = s.state0[0].load(std::memory_order_relaxed);
            const auto z1 = s.state0[1].load(std::memory_order_relaxed);
            const auto d0 = s.dtZero[0].load(std::memory_order_relaxed);
            const auto d1 = s.dtZero[1].load(std::memory_order_relaxed);
            const auto h0 = s.gateHits[0].load(std::memory_order_relaxed);
            const auto h1 = s.gateHits[1].load(std::memory_order_relaxed);
            const auto f0 = s.gateFalse[0].load(std::memory_order_relaxed);
            const auto f1 = s.gateFalse[1].load(std::memory_order_relaxed);
            const auto p0 = s.holderPaused[0].load(std::memory_order_relaxed);
            const auto p1 = s.holderPaused[1].load(std::memory_order_relaxed);
            auto frac = [](std::uint64_t a, std::uint64_t b) { return b ? static_cast<double>(a) / b : -1.0; };
            const bool changed = c0 == 0 ||
                (frac(z0, c0) < 0.5) != (frac(z1, c1) < 0.5) ||
                (frac(d0, c0) > 0.5) != (frac(d1, c1) > 0.5) ||
                (h0 && h1 && (frac(f0, h0) > 0.5) != (frac(f1, h1) > 0.5)) ||
                ((h0 == 0) != (h1 == 0)) ||
                (h0 && h1 && (frac(p0, h0) > 0.5) != (frac(p1, h1) > 0.5));
            if ((pass == 0) != changed)
                continue;
            if (listed++ >= (pass == 0 ? 80u : 20u))
                break;
            Log("[AICOMP]   %s comp=%llX actor=%llX updates=%llu/%llu state0=%llu/%llu state3=%llu/%llu stateOther=%llu/%llu lastState=%u dtZero=%llu/%llu lastDt=%d gate=%llu/%llu gateFalse=%llu/%llu holderPaused=%llu/%llu holderFlags=%02X",
                changed ? "CHANGED" : "same   ", static_cast<unsigned long long>(comp),
                static_cast<unsigned long long>(s.actor.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(c0), static_cast<unsigned long long>(c1),
                static_cast<unsigned long long>(z0), static_cast<unsigned long long>(z1),
                static_cast<unsigned long long>(s.state3[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.state3[1].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.stateOther[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.stateOther[1].load(std::memory_order_relaxed)),
                s.lastState.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(d0), static_cast<unsigned long long>(d1),
                s.lastDt.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(h0), static_cast<unsigned long long>(h1),
                static_cast<unsigned long long>(f0), static_cast<unsigned long long>(f1),
                static_cast<unsigned long long>(p0), static_cast<unsigned long long>(p1),
                s.lastHolderFlags.load(std::memory_order_relaxed));
        }
    }
    if (present - off >= 300)
    {
        g_compArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &CompDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

// [CANAPPLY] Measured: the AI component update's gates passed in
// stereo for every component (state 0, dt nonzero, +0x558 true, holders not
// paused), so the AI graphs are ticked.  Ghidra then found the consumer of
// the pending AI-mode request: FUN_14010ff30 (comp = the actor's vcall+0xD50
// component, pending mode comp+0x94, pending flag +0x9C), called every frame
// by the component update +0x10EFA0 while comp+0x94 != 0.  Its only early
// exit is canApply +0x10FD30 -> +0x5E3CBD0(comp, comp+0xAC, comp+0xAD):
//   comp+0x3F != 0 -> refuse (blocked);
//   interruptible = current mode +0x24 in {0x1015A6B, 0x101F412} || +0xAC ||
//       (mode 0x1004A71 && sub +0x28 == 0x1016B0E) || phase +0x2C > 2 || phase +0x30 > 2;
//   wait = +0xAD && FUN_145e3cf00(comp) (actor-ready check) returns true;
//   apply only if interruptible && !wait.
// DR2 executes at +0x10FF7D (just after canApply returns in the consumer:
// al = answer, rbx = comp) and records the answer with every field it reads.
// DR3 executes at +0x5E3CC49 (just after FUN_145e3cf00 returns: al, r9 = comp).
constexpr std::uintptr_t kCanApplyAnswerRva = 0x10FF7D;
constexpr std::uintptr_t kCanApplyWaitRva = 0x5E3CC49;
constexpr std::size_t kApplySlots = 512;
struct ApplySlot
{
    std::atomic_uintptr_t comp{};
    std::atomic_uint64_t calls[2]{};
    std::atomic_uint64_t refused[2]{};
    std::atomic_uint64_t blocked[2]{};
    std::atomic_uint64_t notInterruptible[2]{};
    std::atomic_uint64_t waitHits[2]{};
    std::atomic_uint64_t waitTrue[2]{};
    std::atomic_uint32_t pending[2]{};
    std::atomic_uint32_t mode[2]{};
    std::atomic_uint32_t sub[2]{};
    std::atomic_uint32_t phase2C[2]{};
    std::atomic_uint32_t phase30[2]{};
    std::atomic_uint32_t flags[2]{}; // 3F | AC<<8 | AD<<16
    std::atomic_uint64_t firstPresent[2]{};
    std::atomic_uintptr_t actor{};
};
ApplySlot g_apply[kApplySlots]{};
std::atomic_bool g_applyArmed{};
std::atomic_uint64_t g_applyOffPresent{};
std::atomic_uint64_t g_applyOverflow{};

ApplySlot* ApplyFind(std::uintptr_t comp)
{
    std::size_t index = static_cast<std::size_t>((comp >> 3) * 0x9E3779B97F4A7C15ull >> 55) &
        (kApplySlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kApplySlots - 1))
    {
        auto& slot = g_apply[index];
        auto existing = slot.comp.load(std::memory_order_acquire);
        if (existing == comp)
            return &slot;
        if (!existing && slot.comp.compare_exchange_strong(existing, comp,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return &slot;
        if (existing == comp)
            return &slot;
    }
    g_applyOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void ApplyRecordAnswer(const CONTEXT* trap)
{
    const auto comp = static_cast<std::uintptr_t>(trap->Rbx);
    if (!comp)
        return;
    auto* slot = ApplyFind(comp);
    if (!slot)
        return;
    const int ph = g_applyOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    const bool yes = (trap->Rax & 0xFF) != 0;
    if (slot->calls[ph].fetch_add(1, std::memory_order_relaxed) == 0)
        slot->firstPresent[ph].store(g_presentCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    std::uint8_t f3F = 0, fAC = 0, fAD = 0;
    std::uint32_t pending = 0, mode = 0, sub = 0, p2C = 0, p30 = 0;
    std::uint64_t actor = 0;
    __try
    {
        const auto* b = reinterpret_cast<const std::uint8_t*>(comp);
        std::memcpy(&actor, b + 0x08, 8);
        std::memcpy(&mode, b + 0x24, 4);
        std::memcpy(&sub, b + 0x28, 4);
        std::memcpy(&p2C, b + 0x2C, 4);
        std::memcpy(&p30, b + 0x30, 4);
        std::memcpy(&pending, b + 0x94, 4);
        f3F = b[0x3F];
        fAC = b[0xAC];
        fAD = b[0xAD];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    slot->actor.store(static_cast<std::uintptr_t>(actor), std::memory_order_relaxed);
    slot->pending[ph].store(pending, std::memory_order_relaxed);
    slot->mode[ph].store(mode, std::memory_order_relaxed);
    slot->sub[ph].store(sub, std::memory_order_relaxed);
    slot->phase2C[ph].store(p2C, std::memory_order_relaxed);
    slot->phase30[ph].store(p30, std::memory_order_relaxed);
    slot->flags[ph].store(f3F | (static_cast<std::uint32_t>(fAC) << 8) | (static_cast<std::uint32_t>(fAD) << 16),
        std::memory_order_relaxed);
    if (!yes)
    {
        slot->refused[ph].fetch_add(1, std::memory_order_relaxed);
        if (f3F)
            slot->blocked[ph].fetch_add(1, std::memory_order_relaxed);
        const bool interruptible = mode == 0x1015A6B || mode == 0x101F412 || fAC ||
            (mode == 0x1004A71 && sub == 0x1016B0E) || p2C > 2 || p30 > 2;
        if (!interruptible)
            slot->notInterruptible[ph].fetch_add(1, std::memory_order_relaxed);
    }
}

void ApplyRecordWait(const CONTEXT* trap)
{
    const auto comp = static_cast<std::uintptr_t>(trap->R9);
    if (!comp)
        return;
    auto* slot = ApplyFind(comp);
    if (!slot)
        return;
    const int ph = g_applyOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    slot->waitHits[ph].fetch_add(1, std::memory_order_relaxed);
    if ((trap->Rax & 0xFF) != 0)
        slot->waitTrue[ph].fetch_add(1, std::memory_order_relaxed);
}

DWORD WINAPI ApplyArmThread(LPVOID)
{
    const auto a = SetExecBreakpointAllThreads(2, g_exeBase + kCanApplyAnswerRva, true);
    const auto b = SetExecBreakpointAllThreads(3, g_exeBase + kCanApplyWaitRva, true);
    g_applyArmed.store(a != 0 && b != 0, std::memory_order_release);
    Log("[CANAPPLY] ARMED: answer +0x%llX on %u threads, actor-ready check +0x%llX on %u threads",
        static_cast<unsigned long long>(kCanApplyAnswerRva), a,
        static_cast<unsigned long long>(kCanApplyWaitRva), b);
    return 0;
}

DWORD WINAPI ApplyDisarmThread(LPVOID)
{
    SetExecBreakpointAllThreads(2, 0, false);
    SetExecBreakpointAllThreads(3, 0, false);
    Log("[CANAPPLY] disarmed");
    return 0;
}

void ApplyTick()
{
    if (!g_applyArmed.load(std::memory_order_acquire))
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto off = g_applyOffPresent.load(std::memory_order_relaxed);
    static std::uint64_t lastSummary = 0;
    const bool stage = off && (present - off == 10 || present - off == 60 || present - off == 300);
    if (present - lastSummary < 300 && !stage)
        return;
    lastSummary = present;
    std::uint64_t comps = 0, calls[2]{}, refused[2]{}, blocked[2]{}, notInt[2]{}, waitT[2]{};
    for (auto& s : g_apply)
    {
        if (!s.comp.load(std::memory_order_acquire))
            continue;
        ++comps;
        for (int ph = 0; ph < 2; ++ph)
        {
            calls[ph] += s.calls[ph].load(std::memory_order_relaxed);
            refused[ph] += s.refused[ph].load(std::memory_order_relaxed);
            blocked[ph] += s.blocked[ph].load(std::memory_order_relaxed);
            notInt[ph] += s.notInterruptible[ph].load(std::memory_order_relaxed);
            waitT[ph] += s.waitTrue[ph].load(std::memory_order_relaxed);
        }
    }
    Log("[CANAPPLY] %s present=%llu components=%llu applyChecks=%llu/%llu refused=%llu/%llu blocked3F=%llu/%llu notInterruptible=%llu/%llu actorNotReady=%llu/%llu overflow=%llu",
        off ? "after-off" : "stereo", static_cast<unsigned long long>(present),
        static_cast<unsigned long long>(comps),
        static_cast<unsigned long long>(calls[0]), static_cast<unsigned long long>(calls[1]),
        static_cast<unsigned long long>(refused[0]), static_cast<unsigned long long>(refused[1]),
        static_cast<unsigned long long>(blocked[0]), static_cast<unsigned long long>(blocked[1]),
        static_cast<unsigned long long>(notInt[0]), static_cast<unsigned long long>(notInt[1]),
        static_cast<unsigned long long>(waitT[0]), static_cast<unsigned long long>(waitT[1]),
        static_cast<unsigned long long>(g_applyOverflow.load(std::memory_order_relaxed)));
    if (!stage && (present % 900) != 0)
        return;
    std::uint32_t listed = 0;
    for (auto& s : g_apply)
    {
        const auto comp = s.comp.load(std::memory_order_acquire);
        if (!comp)
            continue;
        if (listed++ >= 60)
            break;
        char bufs[2][420];
        auto line = [&](int ph) -> const char* {
            const auto fl = s.flags[ph].load(std::memory_order_relaxed);
            char* buf = bufs[ph];
            std::snprintf(buf, sizeof(bufs[ph]),
                "checks=%llu refused=%llu blocked=%llu notInt=%llu wait=%llu/%llu pending=%08X mode=%08X sub=%08X phase2C=%u phase30=%u 3F=%u AC=%u AD=%u",
                static_cast<unsigned long long>(s.calls[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.refused[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.blocked[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.notInterruptible[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.waitTrue[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.waitHits[ph].load(std::memory_order_relaxed)),
                s.pending[ph].load(std::memory_order_relaxed), s.mode[ph].load(std::memory_order_relaxed),
                s.sub[ph].load(std::memory_order_relaxed), s.phase2C[ph].load(std::memory_order_relaxed),
                s.phase30[ph].load(std::memory_order_relaxed), fl & 0xFF, (fl >> 8) & 0xFF, (fl >> 16) & 0xFF);
            return buf;
        };
        const char* stereoLine = line(0);
        const char* afterLine = line(1);
        Log("[CANAPPLY]   comp=%llX actor=%llX | STEREO %s | AFTER-OFF %s",
            static_cast<unsigned long long>(comp),
            static_cast<unsigned long long>(s.actor.load(std::memory_order_relaxed)),
            stereoLine, afterLine);
    }
    if (off && present - off >= 300)
    {
        g_applyArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &ApplyDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

// [MODESW] Measured: canApply is not what holds the sit.  The
// sit character's request component already had PlayMotion (0x1015A6D) as
// its CURRENT mode at phase 2 in stereo; apply checks stopped at the scene
// start because nothing was pending any more.  What never starts is that
// mode's AI graph tray.  The holder tick +0x11D620 switches trays when the
// requested mode differs from the current one:
//   requested = FUN_145e74db0(holder): real only if ctl = holder+0x30 exists,
//       holder[0] exists, ctl+0x480 != 0 and ctl+0x1C == 2, then built from
//       ctl+0x10 (+0x5E7B050); otherwise "none" (+0x5E79E00).
//   if requested != holder+0x20: FUN_145e74b30 deactivates the current tray
//       and creates/activates the new one via +0x5E74F50(holder, a, b, 1).
// DR2 executes at +0x11D6BA (after requested is computed: rbx = holder,
// [rsp+0x28] = requested) and reads requested, current and the controller's
// state; DR3 executes at +0x5E74BFB (rax = new tray or 0, rdi = holder).
constexpr std::uintptr_t kModeSwReqRva = 0x11D6BA;
constexpr std::uintptr_t kModeSwTrayRva = 0x5E74BFB;
constexpr std::size_t kHolderSlots = 1024;
struct HolderSlot
{
    std::atomic_uintptr_t holder{};
    std::atomic_uintptr_t owner{};
    std::atomic_uint64_t ticks[2]{};
    std::atomic_uint64_t differs[2]{};
    std::atomic_uint64_t ctlRunning[2]{};
    std::atomic_uint64_t ctlNull[2]{};
    std::atomic_uint64_t ctlEmpty[2]{};
    std::atomic_uint32_t lastReq[2]{};
    std::atomic_uint32_t lastCur[2]{};
    std::atomic_uint32_t lastCtlState[2]{};
    std::atomic_uint32_t lastCtlId[2]{};
    std::atomic_uint64_t lastCtlCount[2]{};
    std::atomic_uint64_t trayCreates[2]{};
    std::atomic_uint64_t trayFails[2]{};
    std::atomic_uint64_t firstPresent[2]{};
};
HolderSlot g_holders[kHolderSlots]{};
std::atomic_bool g_modeSwArmed{};
std::atomic_uint64_t g_modeSwOffPresent{};
std::atomic_uint64_t g_modeSwOverflow{};

HolderSlot* HolderFind(std::uintptr_t holder)
{
    std::size_t index = static_cast<std::size_t>((holder >> 3) * 0x9E3779B97F4A7C15ull >> 54) &
        (kHolderSlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kHolderSlots - 1))
    {
        auto& slot = g_holders[index];
        auto existing = slot.holder.load(std::memory_order_acquire);
        if (existing == holder)
            return &slot;
        if (!existing && slot.holder.compare_exchange_strong(existing, holder,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return &slot;
        if (existing == holder)
            return &slot;
    }
    g_modeSwOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void ModeSwRecordReq(const CONTEXT* trap)
{
    const auto holder = static_cast<std::uintptr_t>(trap->Rbx);
    if (!holder)
        return;
    auto* slot = HolderFind(holder);
    if (!slot)
        return;
    const int ph = g_modeSwOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    if (slot->ticks[ph].fetch_add(1, std::memory_order_relaxed) == 0)
        slot->firstPresent[ph].store(g_presentCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    std::uint32_t req = 0, cur = 0, ctlState = 0xFFFFFFFF, ctlId = 0;
    std::uint64_t owner = 0, ctl = 0, ctlCount = 0;
    __try
    {
        req = *reinterpret_cast<const std::uint32_t*>(trap->Rsp + 0x28);
        const auto* h = reinterpret_cast<const std::uint8_t*>(holder);
        std::memcpy(&owner, h + 0x00, 8);
        std::memcpy(&cur, h + 0x20, 4);
        std::memcpy(&ctl, h + 0x30, 8);
        if (ctl)
        {
            const auto* c = reinterpret_cast<const std::uint8_t*>(ctl);
            std::memcpy(&ctlId, c + 0x10, 4);
            std::memcpy(&ctlState, c + 0x1C, 4);
            std::memcpy(&ctlCount, c + 0x480, 8);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    slot->owner.store(static_cast<std::uintptr_t>(owner), std::memory_order_relaxed);
    slot->lastReq[ph].store(req, std::memory_order_relaxed);
    slot->lastCur[ph].store(cur, std::memory_order_relaxed);
    slot->lastCtlState[ph].store(ctlState, std::memory_order_relaxed);
    slot->lastCtlId[ph].store(ctlId, std::memory_order_relaxed);
    slot->lastCtlCount[ph].store(ctlCount, std::memory_order_relaxed);
    if (req != cur)
        slot->differs[ph].fetch_add(1, std::memory_order_relaxed);
    if (!ctl)
        slot->ctlNull[ph].fetch_add(1, std::memory_order_relaxed);
    else if (!ctlCount)
        slot->ctlEmpty[ph].fetch_add(1, std::memory_order_relaxed);
    if (ctl && ctlState == 2)
        slot->ctlRunning[ph].fetch_add(1, std::memory_order_relaxed);
}

void ModeSwRecordTray(const CONTEXT* trap)
{
    const auto holder = static_cast<std::uintptr_t>(trap->Rdi);
    if (!holder)
        return;
    auto* slot = HolderFind(holder);
    if (!slot)
        return;
    const int ph = g_modeSwOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    slot->trayCreates[ph].fetch_add(1, std::memory_order_relaxed);
    if (!trap->Rax)
        slot->trayFails[ph].fetch_add(1, std::memory_order_relaxed);
}

DWORD WINAPI ModeSwArmThread(LPVOID)
{
    const auto a = SetExecBreakpointAllThreads(2, g_exeBase + kModeSwReqRva, true);
    const auto b = SetExecBreakpointAllThreads(3, g_exeBase + kModeSwTrayRva, true);
    g_modeSwArmed.store(a != 0 && b != 0, std::memory_order_release);
    Log("[MODESW] ARMED: requested-mode read +0x%llX on %u threads, tray activation +0x%llX on %u threads",
        static_cast<unsigned long long>(kModeSwReqRva), a,
        static_cast<unsigned long long>(kModeSwTrayRva), b);
    return 0;
}

DWORD WINAPI ModeSwDisarmThread(LPVOID)
{
    SetExecBreakpointAllThreads(2, 0, false);
    SetExecBreakpointAllThreads(3, 0, false);
    Log("[MODESW] disarmed");
    return 0;
}

void ModeSwTick()
{
    if (!g_modeSwArmed.load(std::memory_order_acquire))
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto off = g_modeSwOffPresent.load(std::memory_order_relaxed);
    static std::uint64_t lastSummary = 0;
    const bool stage = off && (present - off == 10 || present - off == 60 || present - off == 300);
    if (present - lastSummary < 300 && !stage)
        return;
    lastSummary = present;
    std::uint64_t holders = 0, ticks[2]{}, differs[2]{}, running[2]{}, creates[2]{}, fails[2]{};
    for (auto& s : g_holders)
    {
        if (!s.holder.load(std::memory_order_acquire))
            continue;
        ++holders;
        for (int ph = 0; ph < 2; ++ph)
        {
            ticks[ph] += s.ticks[ph].load(std::memory_order_relaxed);
            differs[ph] += s.differs[ph].load(std::memory_order_relaxed);
            running[ph] += s.ctlRunning[ph].load(std::memory_order_relaxed);
            creates[ph] += s.trayCreates[ph].load(std::memory_order_relaxed);
            fails[ph] += s.trayFails[ph].load(std::memory_order_relaxed);
        }
    }
    Log("[MODESW] %s present=%llu holders=%llu ticks=%llu/%llu requestedDiffers=%llu/%llu controllerRunning=%llu/%llu trayActivations=%llu/%llu trayFailed=%llu/%llu overflow=%llu",
        off ? "after-off" : "stereo", static_cast<unsigned long long>(present),
        static_cast<unsigned long long>(holders),
        static_cast<unsigned long long>(ticks[0]), static_cast<unsigned long long>(ticks[1]),
        static_cast<unsigned long long>(differs[0]), static_cast<unsigned long long>(differs[1]),
        static_cast<unsigned long long>(running[0]), static_cast<unsigned long long>(running[1]),
        static_cast<unsigned long long>(creates[0]), static_cast<unsigned long long>(creates[1]),
        static_cast<unsigned long long>(fails[0]), static_cast<unsigned long long>(fails[1]),
        static_cast<unsigned long long>(g_modeSwOverflow.load(std::memory_order_relaxed)));
    if (!stage && (present % 900) != 0)
        return;
    std::uint32_t listed = 0;
    for (auto& s : g_holders)
    {
        const auto holder = s.holder.load(std::memory_order_acquire);
        if (!holder)
            continue;
        // only holders that ever had a controller with entries, or ever switched
        const bool interesting = s.lastCtlCount[0].load(std::memory_order_relaxed) ||
            s.lastCtlCount[1].load(std::memory_order_relaxed) ||
            s.differs[0].load(std::memory_order_relaxed) || s.differs[1].load(std::memory_order_relaxed) ||
            s.trayCreates[0].load(std::memory_order_relaxed) || s.trayCreates[1].load(std::memory_order_relaxed);
        if (!interesting)
            continue;
        if (listed++ >= 60)
            break;
        char bufs[2][300];
        for (int ph = 0; ph < 2; ++ph)
            std::snprintf(bufs[ph], sizeof(bufs[ph]),
                "ticks=%llu differs=%llu ctlRunning=%llu ctlNull=%llu ctlEmpty=%llu req=%08X cur=%08X ctlState=%u ctlId=%08X ctlCount=%llu trays=%llu/%llufail",
                static_cast<unsigned long long>(s.ticks[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.differs[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.ctlRunning[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.ctlNull[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.ctlEmpty[ph].load(std::memory_order_relaxed)),
                s.lastReq[ph].load(std::memory_order_relaxed), s.lastCur[ph].load(std::memory_order_relaxed),
                s.lastCtlState[ph].load(std::memory_order_relaxed), s.lastCtlId[ph].load(std::memory_order_relaxed),
                static_cast<unsigned long long>(s.lastCtlCount[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.trayCreates[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.trayFails[ph].load(std::memory_order_relaxed)));
        Log("[MODESW]   holder=%llX owner=%llX | STEREO %s | AFTER-OFF %s",
            static_cast<unsigned long long>(holder),
            static_cast<unsigned long long>(s.owner.load(std::memory_order_relaxed)), bufs[0], bufs[1]);
    }
    if (off && present - off >= 300)
    {
        g_modeSwArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &ModeSwDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

// [TASKSKIP] Measured: three AI graph holders ticked only 5-11
// times across the whole stereo hang (others hundreds to thousands) and
// steadily after stereo-off - the conversation characters' whole update is
// starved, as the [AICOMP]/[ACTUPD] rows with 3-11 stereo updates already
// showed.  Per-actor updates are engine tasks: runner +0xEE69FC0 -> invoker
// +0x2DC1D60 (dt += task+0x4C banked time) -> [task+8](task+0x28 = actor),
// where the actor update task's callback is the thunk +0xF3B50 -> +0x5DC7060.
// The task frame-skip rule +0xEE35030 (decompiled):
//   counter +0x4A, interval +0x49, flags word +0x38 (0x40 run-now, 0x80 skip-once);
//   if (counter >= interval || run-now) and !skip-once: counter reset, RUN;
//   else counter++, +0x4C += frame dt, clear skip-once, SKIP.
// DR2 executes at +0xEE35030 (rcx = task).  Actor update tasks (callback
// +0xF3B50) are tallied per actor: runs vs skips, interval, counter, flags,
// in stereo and after stereo-off; the callbacks of all other tasks seen are
// counted too, and the first skips of actor tasks capture call stacks.
constexpr std::uintptr_t kTaskSkipRuleRva = 0xEE35030;
constexpr std::uintptr_t kActorUpdateThunkRva = 0xF3B50;
constexpr std::size_t kTaskSlots = 512;
struct TaskSlot
{
    std::atomic_uintptr_t task{};
    std::atomic_uintptr_t actor{};
    std::atomic_uint64_t runs[2]{};
    std::atomic_uint64_t skips[2]{};
    std::atomic_uint64_t skipOnce[2]{};
    std::atomic_uint64_t runNow[2]{};
    std::atomic_uint32_t interval[2]{};
    std::atomic_uint32_t counter[2]{};
    std::atomic_uint32_t flags[2]{};
    std::atomic_uint32_t maxInterval[2]{};
};
TaskSlot g_tasks[kTaskSlots]{};
constexpr std::size_t kCallbackSlots = 64;
std::atomic_uintptr_t g_taskCallbacks[kCallbackSlots]{};
std::atomic_uint64_t g_taskCallbackHits[kCallbackSlots]{};
std::atomic_bool g_taskSkipArmed{};
std::atomic_uint64_t g_taskSkipOffPresent{};
std::atomic_uint64_t g_taskOverflow{};
std::atomic_uint32_t g_taskStacks[2]{};

void TaskSkipRecord(const CONTEXT* trap)
{
    const auto task = static_cast<std::uintptr_t>(trap->Rcx);
    if (!task)
        return;
    std::uint64_t callback = 0, actor = 0;
    std::uint16_t flags = 0;
    std::uint8_t interval = 0, counter = 0;
    __try
    {
        const auto* b = reinterpret_cast<const std::uint8_t*>(task);
        std::memcpy(&callback, b + 0x08, 8);
        std::memcpy(&actor, b + 0x28, 8);
        std::memcpy(&flags, b + 0x38, 2);
        interval = b[0x49];
        counter = b[0x4A];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    const auto cbRva = callback >= g_exeBase && callback < g_exeBase + kRetailImageSize ? callback - g_exeBase : 0;
    for (std::size_t i = 0; i < kCallbackSlots; ++i)
    {
        auto existing = g_taskCallbacks[i].load(std::memory_order_relaxed);
        if (existing == cbRva || (!existing && g_taskCallbacks[i].compare_exchange_strong(existing, cbRva)))
        {
            g_taskCallbackHits[i].fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (existing == cbRva)
        {
            g_taskCallbackHits[i].fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    if (cbRva != kActorUpdateThunkRva)
        return;
    const bool runNow = (flags & 0x40) != 0;
    const bool skipOnce = (flags & 0x80) != 0;
    const bool run = (counter >= interval || runNow) && !skipOnce;
    const int ph = g_taskSkipOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    std::size_t index = static_cast<std::size_t>((task >> 3) * 0x9E3779B97F4A7C15ull >> 55) & (kTaskSlots - 1);
    TaskSlot* slot = nullptr;
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kTaskSlots - 1))
    {
        auto& s = g_tasks[index];
        auto existing = s.task.load(std::memory_order_acquire);
        if (existing == task || (!existing && s.task.compare_exchange_strong(existing, task)) || existing == task)
        {
            slot = &s;
            break;
        }
    }
    if (!slot)
    {
        g_taskOverflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    slot->actor.store(static_cast<std::uintptr_t>(actor), std::memory_order_relaxed);
    (run ? slot->runs[ph] : slot->skips[ph]).fetch_add(1, std::memory_order_relaxed);
    if (skipOnce)
        slot->skipOnce[ph].fetch_add(1, std::memory_order_relaxed);
    if (runNow)
        slot->runNow[ph].fetch_add(1, std::memory_order_relaxed);
    slot->interval[ph].store(interval, std::memory_order_relaxed);
    slot->counter[ph].store(counter, std::memory_order_relaxed);
    slot->flags[ph].store(flags, std::memory_order_relaxed);
    auto maxI = slot->maxInterval[ph].load(std::memory_order_relaxed);
    while (interval > maxI && !slot->maxInterval[ph].compare_exchange_weak(maxI, interval))
    {
    }
    if (!run && g_taskStacks[ph].load(std::memory_order_relaxed) < 4 &&
        g_taskStacks[ph].fetch_add(1, std::memory_order_relaxed) < 4)
        TraceCapture(static_cast<std::uint32_t>(ph), trap, task, actor, (static_cast<std::uint64_t>(interval) << 16) | counter);
}

// [ACTRATE] Measured: every actor update task that went through
// the skip rule ran at the normal rate for its interval in stereo (interval
// 0/1/6/10/15/30), and none showed the 5-11-updates-per-hang pattern.  So
// the starved conversation characters' updates never reach that rule.  DR3
// executes at the actor update job +0x5DC7060 (rcx = actor) and counts
// updates per actor; the report joins each actor's update RATE (per present,
// stereo vs after stereo-off) with its skip-rule task row if it has one, and
// lists actors by how much faster they update after stereo-off.
constexpr std::uintptr_t kActorUpdateJobRva = 0x5DC7060;
constexpr std::size_t kRateSlots = 1024;
struct RateSlot
{
    std::atomic_uintptr_t actor{};
    std::atomic_uint64_t updates[2]{};
    std::atomic_uint64_t firstPresent[2]{};
    std::atomic_uint64_t lastPresent[2]{};
};
RateSlot g_rates[kRateSlots]{};
std::atomic_uint64_t g_rateOverflow{};
std::atomic_uint64_t g_taskSkipArmPresent{};

void ActRateRecord(const CONTEXT* trap)
{
    const auto actor = static_cast<std::uintptr_t>(trap->Rcx);
    if (!actor)
        return;
    const int ph = g_taskSkipOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    std::size_t index = static_cast<std::size_t>((actor >> 3) * 0x9E3779B97F4A7C15ull >> 54) & (kRateSlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kRateSlots - 1))
    {
        auto& s = g_rates[index];
        auto existing = s.actor.load(std::memory_order_acquire);
        if (existing == actor || (!existing && s.actor.compare_exchange_strong(existing, actor)) || existing == actor)
        {
            if (s.updates[ph].fetch_add(1, std::memory_order_relaxed) == 0)
                s.firstPresent[ph].store(present, std::memory_order_relaxed);
            s.lastPresent[ph].store(present, std::memory_order_relaxed);
            return;
        }
    }
    g_rateOverflow.fetch_add(1, std::memory_order_relaxed);
}

// [TASKHOP] Measured: the conversation characters' tasks were approved RUN
// by the skip rule ~1130 times in stereo but their update job ran 5-11 times,
// all before the scripted camera.  This counts the same tasks at the invoker
// +0x2DC1D60 (rcx = task, rdx = &dt): it skips the callback when *dt == 0
// (+0x2DC1D6D -> +0x176DCE0) and dispatches by kind task+0x30.  Per actor
// update task (callback +0xF3B50): invoker hits, zero-dt hits, last dt, kind,
// flags word +0x38 (bit 2 picks world+0x208 over +0x204), byte +0x39, and the
// dt pointer, in stereo vs after stereo-off; joined into the [ACTRATE] rows.
constexpr std::uintptr_t kTaskInvokerRva = 0x2DC1D60;
constexpr std::size_t kHopSlots = 512;
struct HopSlot
{
    std::atomic_uintptr_t task{};
    std::atomic_uintptr_t actor{};
    std::atomic_uint64_t hits[2]{};
    std::atomic_uint64_t dtZero[2]{};
    std::atomic<std::int32_t> lastDt[2]{};
    std::atomic_uint64_t kind[2]{};
    std::atomic_uint32_t flags[2]{};
    std::atomic_uint32_t byte39[2]{};
    std::atomic_uintptr_t dtPtr[2]{};
};
HopSlot g_hops[kHopSlots]{};
std::atomic_uint64_t g_hopOverflow{};

void TaskHopRecord(const CONTEXT* trap)
{
    const auto task = static_cast<std::uintptr_t>(trap->Rcx);
    if (!task)
        return;
    std::uint64_t callback = 0, actor = 0, kind = 0;
    std::uint16_t flags = 0;
    std::uint8_t b39 = 0;
    std::int32_t dt = 0;
    __try
    {
        const auto* b = reinterpret_cast<const std::uint8_t*>(task);
        std::memcpy(&callback, b + 0x08, 8);
        if (callback != g_exeBase + kActorUpdateThunkRva)
            return;
        std::memcpy(&actor, b + 0x28, 8);
        std::memcpy(&kind, b + 0x30, 8);
        std::memcpy(&flags, b + 0x38, 2);
        b39 = b[0x39];
        if (trap->Rdx)
            dt = *reinterpret_cast<const std::int32_t*>(trap->Rdx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    const int ph = g_taskSkipOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    std::size_t index = static_cast<std::size_t>((task >> 3) * 0x9E3779B97F4A7C15ull >> 55) & (kHopSlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kHopSlots - 1))
    {
        auto& s = g_hops[index];
        auto existing = s.task.load(std::memory_order_acquire);
        if (existing == task || (!existing && s.task.compare_exchange_strong(existing, task)) || existing == task)
        {
            s.actor.store(static_cast<std::uintptr_t>(actor), std::memory_order_relaxed);
            s.hits[ph].fetch_add(1, std::memory_order_relaxed);
            if (dt == 0)
                s.dtZero[ph].fetch_add(1, std::memory_order_relaxed);
            s.lastDt[ph].store(dt, std::memory_order_relaxed);
            s.kind[ph].store(kind, std::memory_order_relaxed);
            s.flags[ph].store(flags, std::memory_order_relaxed);
            s.byte39[ph].store(b39, std::memory_order_relaxed);
            s.dtPtr[ph].store(static_cast<std::uintptr_t>(trap->Rdx), std::memory_order_relaxed);
            return;
        }
    }
    g_hopOverflow.fetch_add(1, std::memory_order_relaxed);
}

void ActRateReport(std::uint64_t present, std::uint64_t off)
{
    const auto arm = g_taskSkipArmPresent.load(std::memory_order_relaxed);
    const double stereoPresents = off > arm ? static_cast<double>(off - arm) : 1.0;
    const double afterPresents = present > off ? static_cast<double>(present - off) : 1.0;
    struct Row { RateSlot* s; double rs; double ra; double ratio; };
    Row rows[kRateSlots];
    std::size_t count = 0;
    for (auto& s : g_rates)
    {
        if (!s.actor.load(std::memory_order_acquire))
            continue;
        const double rs = s.updates[0].load(std::memory_order_relaxed) / stereoPresents;
        const double ra = s.updates[1].load(std::memory_order_relaxed) / afterPresents;
        rows[count++] = Row{&s, rs, ra, (ra + 1e-3) / (rs + 1e-3)};
    }
    std::sort(rows, rows + count, [](const Row& a, const Row& b) { return a.ratio > b.ratio; });
    Log("[ACTRATE] %zu actors updated; stereo presents=%.0f after-off presents=%.0f overflow=%llu",
        count, stereoPresents, afterPresents,
        static_cast<unsigned long long>(g_rateOverflow.load(std::memory_order_relaxed)));
    for (std::size_t i = 0; i < count && i < 40; ++i)
    {
        const auto actor = rows[i].s->actor.load(std::memory_order_relaxed);
        TaskSlot* task = nullptr;
        for (auto& ts : g_tasks)
            if (ts.task.load(std::memory_order_relaxed) && ts.actor.load(std::memory_order_relaxed) == actor)
            {
                task = &ts;
                break;
            }
        HopSlot* hop = nullptr;
        for (auto& hs : g_hops)
            if (hs.task.load(std::memory_order_relaxed) && hs.actor.load(std::memory_order_relaxed) == actor)
            {
                hop = &hs;
                break;
            }
        char taskInfo[600] = "NO task seen at the invoker";
        if (hop)
            std::snprintf(taskInfo, sizeof(taskInfo),
                "task=%llX INVOKER stereo hits=%llu dtZero=%llu lastDt=%d kind=%llu flags=%04X b39=%u dtPtr=%llX | after hits=%llu dtZero=%llu lastDt=%d kind=%llu flags=%04X b39=%u dtPtr=%llX",
                static_cast<unsigned long long>(hop->task.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(hop->hits[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(hop->dtZero[0].load(std::memory_order_relaxed)),
                hop->lastDt[0].load(std::memory_order_relaxed),
                static_cast<unsigned long long>(hop->kind[0].load(std::memory_order_relaxed)),
                hop->flags[0].load(std::memory_order_relaxed), hop->byte39[0].load(std::memory_order_relaxed),
                static_cast<unsigned long long>(hop->dtPtr[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(hop->hits[1].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(hop->dtZero[1].load(std::memory_order_relaxed)),
                hop->lastDt[1].load(std::memory_order_relaxed),
                static_cast<unsigned long long>(hop->kind[1].load(std::memory_order_relaxed)),
                hop->flags[1].load(std::memory_order_relaxed), hop->byte39[1].load(std::memory_order_relaxed),
                static_cast<unsigned long long>(hop->dtPtr[1].load(std::memory_order_relaxed)));
        if (false && task)
            std::snprintf(taskInfo, sizeof(taskInfo),
                "task=%llX stereo runs/skips=%llu/%llu interval=%u runNow=%llu | after runs/skips=%llu/%llu interval=%u",
                static_cast<unsigned long long>(task->task.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(task->runs[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(task->skips[0].load(std::memory_order_relaxed)),
                task->interval[0].load(std::memory_order_relaxed),
                static_cast<unsigned long long>(task->runNow[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(task->runs[1].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(task->skips[1].load(std::memory_order_relaxed)),
                task->interval[1].load(std::memory_order_relaxed));
        char name[160] = "";
        __try
        {
            const auto vtable = *reinterpret_cast<const std::uint64_t*>(actor);
            if (vtable >= g_exeBase && vtable < g_exeBase + kRetailImageSize)
                SeqCensusClassName(vtable - g_exeBase, name, sizeof(name));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        Log("[ACTRATE]   #%zu actor=%llX %s updates stereo/after=%llu/%llu rate=%.3f/%.3f per present (x%.1f) first=%llu/%llu last=%llu/%llu | %s",
            i + 1, static_cast<unsigned long long>(actor), name[0] ? name : "?",
            static_cast<unsigned long long>(rows[i].s->updates[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(rows[i].s->updates[1].load(std::memory_order_relaxed)),
            rows[i].rs, rows[i].ra, rows[i].ratio,
            static_cast<unsigned long long>(rows[i].s->firstPresent[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(rows[i].s->firstPresent[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(rows[i].s->lastPresent[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(rows[i].s->lastPresent[1].load(std::memory_order_relaxed)),
            taskInfo);
    }
}

// [POPGATE] Measured: the starved conversation tasks reach the invoker exactly
// as often as their job runs (5/8/11), dt never 0 -> the drop is before the
// invoker.  Runner +0xEE69FC0 calls the invoker unconditionally; the queue pop
// (+0x2DCCFE0 / +0x2DCD110) calls the skip rule and, when it says RUN (al=0),
// still completes the task WITHOUT running it when the scheduler flag
// (scheduler+0x1FC != 0, passed in dl) is set and the batch lacks flag 0x10.
// Probes both copies of that check: +0x2DCD08A (rdi task, rbx batch, bpl flag,
// rsi scheduler) and +0x2DCD172 (rdi task, rbx batch, sil flag).
constexpr std::uintptr_t kPopGateWrapRva = 0x2DCD08A;
constexpr std::uintptr_t kPopGateLoopRva = 0x2DCD172;
constexpr std::size_t kGateSlots = 1024;
struct GateSlot
{
    std::atomic_uintptr_t task{};
    std::atomic_uintptr_t actor{};
    std::atomic_uint64_t hits[2]{};
    std::atomic_uint64_t restricted[2]{};
    std::atomic_uint64_t dropped[2]{};
    std::atomic_uint64_t loopPath[2]{};
    std::atomic_uintptr_t batch[2]{};
    std::atomic_uint32_t batchFlags[2]{};
    std::atomic_uint32_t batchCount[2]{};
    std::atomic_uint64_t firstPresent[2]{};
    std::atomic_uint64_t lastPresent[2]{};
    std::atomic_uint64_t lastRestrictedPresent[2]{};
};
GateSlot g_gates[kGateSlots]{};
std::atomic_uint64_t g_gateOverflow{};
std::atomic_uint64_t g_gateAllHits[2]{};
std::atomic_uint64_t g_gateAllRestricted[2]{};
std::atomic_uint64_t g_gateAllDropped[2]{};
std::atomic_uintptr_t g_gateSched[4]{};
std::atomic_uint32_t g_gateSched1FC[4]{};
std::atomic_uint32_t g_gateSched1F8[4]{};
std::atomic_uint64_t g_gateSchedHits[4]{};
std::atomic_uint64_t g_gateSchedNonzero[4]{};

void PopGateRecord(const CONTEXT* trap, bool loopPath)
{
    const auto task = static_cast<std::uintptr_t>(trap->Rdi);
    const auto batch = static_cast<std::uintptr_t>(trap->Rbx);
    const bool restricted = (loopPath ? (trap->Rsi & 0xFF) : (trap->Rbp & 0xFF)) != 0;
    if (!task || !batch)
        return;
    std::uint64_t callback = 0, actor = 0;
    std::uint8_t bflags = 0;
    std::uint32_t bcount = 0;
    __try
    {
        const auto* b = reinterpret_cast<const std::uint8_t*>(task);
        std::memcpy(&callback, b + 0x08, 8);
        std::memcpy(&actor, b + 0x28, 8);
        bflags = *reinterpret_cast<const std::uint8_t*>(batch);
        std::memcpy(&bcount, reinterpret_cast<const std::uint8_t*>(batch) + 0x70, 4);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
    const bool dropped = restricted && (bflags & 0x10) == 0;
    const int ph = g_taskSkipOffPresent.load(std::memory_order_relaxed) ? 1 : 0;
    g_gateAllHits[ph].fetch_add(1, std::memory_order_relaxed);
    if (restricted)
        g_gateAllRestricted[ph].fetch_add(1, std::memory_order_relaxed);
    if (dropped)
        g_gateAllDropped[ph].fetch_add(1, std::memory_order_relaxed);
    if (!loopPath)
    {
        const auto sched = static_cast<std::uintptr_t>(trap->Rsi);
        std::uint32_t v1fc = 0;
        std::uint8_t v1f8 = 0;
        bool ok = false;
        __try
        {
            std::memcpy(&v1fc, reinterpret_cast<const std::uint8_t*>(sched) + 0x1FC, 4);
            v1f8 = *(reinterpret_cast<const std::uint8_t*>(sched) + 0x1F8);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (ok)
            for (int i = 0; i < 4; ++i)
            {
                auto e = g_gateSched[i].load(std::memory_order_relaxed);
                if (e == sched || (!e && g_gateSched[i].compare_exchange_strong(e, sched)) || e == sched)
                {
                    g_gateSched1FC[i].store(v1fc, std::memory_order_relaxed);
                    g_gateSched1F8[i].store(v1f8, std::memory_order_relaxed);
                    g_gateSchedHits[i].fetch_add(1, std::memory_order_relaxed);
                    if (v1fc)
                        g_gateSchedNonzero[i].fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
    }
    if (callback != g_exeBase + kActorUpdateThunkRva)
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    std::size_t index = static_cast<std::size_t>((task >> 3) * 0x9E3779B97F4A7C15ull >> 54) & (kGateSlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kGateSlots - 1))
    {
        auto& s = g_gates[index];
        auto existing = s.task.load(std::memory_order_acquire);
        if (existing == task || (!existing && s.task.compare_exchange_strong(existing, task)) || existing == task)
        {
            s.actor.store(static_cast<std::uintptr_t>(actor), std::memory_order_relaxed);
            if (!s.hits[ph].fetch_add(1, std::memory_order_relaxed))
                s.firstPresent[ph].store(present, std::memory_order_relaxed);
            s.lastPresent[ph].store(present, std::memory_order_relaxed);
            if (restricted)
            {
                s.restricted[ph].fetch_add(1, std::memory_order_relaxed);
                s.lastRestrictedPresent[ph].store(present, std::memory_order_relaxed);
            }
            if (dropped)
                s.dropped[ph].fetch_add(1, std::memory_order_relaxed);
            if (loopPath)
                s.loopPath[ph].fetch_add(1, std::memory_order_relaxed);
            s.batch[ph].store(batch, std::memory_order_relaxed);
            s.batchFlags[ph].store(bflags, std::memory_order_relaxed);
            s.batchCount[ph].store(bcount, std::memory_order_relaxed);
            return;
        }
    }
    g_gateOverflow.fetch_add(1, std::memory_order_relaxed);
}

void PopGateReport(std::uint64_t present, std::uint64_t off)
{
    Log("[POPGATE] present=%llu off=%llu all pops-after-RUN stereo/after=%llu/%llu restricted=%llu/%llu DROPPED=%llu/%llu overflow=%llu",
        static_cast<unsigned long long>(present), static_cast<unsigned long long>(off),
        static_cast<unsigned long long>(g_gateAllHits[0].load()), static_cast<unsigned long long>(g_gateAllHits[1].load()),
        static_cast<unsigned long long>(g_gateAllRestricted[0].load()), static_cast<unsigned long long>(g_gateAllRestricted[1].load()),
        static_cast<unsigned long long>(g_gateAllDropped[0].load()), static_cast<unsigned long long>(g_gateAllDropped[1].load()),
        static_cast<unsigned long long>(g_gateOverflow.load()));
    for (int i = 0; i < 4; ++i)
        if (const auto sched = g_gateSched[i].load())
            Log("[POPGATE]   scheduler=%llX hits=%llu +0x1FC nonzero=%llu last+0x1FC=%u last+0x1F8=%u",
                static_cast<unsigned long long>(sched), static_cast<unsigned long long>(g_gateSchedHits[i].load()),
                static_cast<unsigned long long>(g_gateSchedNonzero[i].load()), g_gateSched1FC[i].load(), g_gateSched1F8[i].load());
    struct Row { GateSlot* s; std::uint64_t key; };
    static Row rows[kGateSlots];
    std::size_t count = 0;
    for (auto& s : g_gates)
        if (s.task.load(std::memory_order_acquire))
            rows[count++] = Row{&s, s.dropped[0].load() * 4 + s.restricted[0].load()};
    std::sort(rows, rows + count, [](const Row& a, const Row& b) { return a.key > b.key; });
    Log("[POPGATE] %zu actor update tasks passed the rule", count);
    for (std::size_t i = 0; i < count && i < 30; ++i)
    {
        const auto& s = *rows[i].s;
        const auto actor = s.actor.load();
        char name[160] = "";
        __try
        {
            const auto vtable = *reinterpret_cast<const std::uint64_t*>(actor);
            if (vtable >= g_exeBase && vtable < g_exeBase + kRetailImageSize)
                SeqCensusClassName(vtable - g_exeBase, name, sizeof(name));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        char ph[2][300];
        for (int k = 0; k < 2; ++k)
            std::snprintf(ph[k], sizeof(ph[k]),
                "hits=%llu restricted=%llu DROPPED=%llu loopPath=%llu batch=%llX flags=%02X count=%u first/last=%llu/%llu lastRestricted=%llu",
                static_cast<unsigned long long>(s.hits[k].load()), static_cast<unsigned long long>(s.restricted[k].load()),
                static_cast<unsigned long long>(s.dropped[k].load()), static_cast<unsigned long long>(s.loopPath[k].load()),
                static_cast<unsigned long long>(s.batch[k].load()), s.batchFlags[k].load(), s.batchCount[k].load(),
                static_cast<unsigned long long>(s.firstPresent[k].load()), static_cast<unsigned long long>(s.lastPresent[k].load()),
                static_cast<unsigned long long>(s.lastRestrictedPresent[k].load()));
        Log("[POPGATE]   #%zu task=%llX actor=%llX %s | STEREO %s | AFTER %s", i + 1,
            static_cast<unsigned long long>(s.task.load()), static_cast<unsigned long long>(actor), name[0] ? name : "?", ph[0], ph[1]);
    }
}

DWORD WINAPI TaskSkipArmThread(LPVOID)
{
    {
        const auto h = SetExecBreakpointAllThreads(2, g_exeBase + kPopGateWrapRva, true);
        const auto r = SetExecBreakpointAllThreads(3, g_exeBase + kPopGateLoopRva, true);
        g_taskSkipArmed.store(h != 0 && r != 0, std::memory_order_release);
        Log("[POPGATE] ARMED: pop gate +0x%llX on %u threads, +0x%llX on %u threads",
            static_cast<unsigned long long>(kPopGateWrapRva), h,
            static_cast<unsigned long long>(kPopGateLoopRva), r);
        return 0;
    }
    const auto r = SetExecBreakpointAllThreads(3, g_exeBase + kActorUpdateJobRva, true);
    Log("[ACTRATE] actor update job +0x%llX armed on %u threads",
        static_cast<unsigned long long>(kActorUpdateJobRva), r);
    const auto a = SetExecBreakpointAllThreads(2, g_exeBase + kTaskSkipRuleRva, true);
    g_taskSkipArmed.store(a != 0, std::memory_order_release);
    Log("[TASKSKIP] ARMED: task frame-skip rule +0x%llX on %u threads (actor update callback +0x%llX)",
        static_cast<unsigned long long>(kTaskSkipRuleRva), a,
        static_cast<unsigned long long>(kActorUpdateThunkRva));
    return 0;
}

DWORD WINAPI TaskSkipDisarmThread(LPVOID)
{
    SetExecBreakpointAllThreads(2, 0, false);
    SetExecBreakpointAllThreads(3, 0, false);
    Log("[TASKSKIP] disarmed");
    return 0;
}

void TaskSkipTick()
{
    if (!g_taskSkipArmed.load(std::memory_order_acquire))
        return;
    // stacks
    {
        const auto written = g_traceWritten.load(std::memory_order_acquire);
        auto logged = g_traceLogged.load(std::memory_order_relaxed);
        while (logged < written)
        {
            const auto& e = g_trace[logged];
            char line[1500];
            int n = std::snprintf(line, sizeof(line),
                "[TASKSKIP] actor-task SKIP %s present=%llu task=%llX actor=%llX interval=%llu counter=%llu tid=%u stack:",
                e.site ? "AFTER-OFF" : "in-stereo", static_cast<unsigned long long>(e.present),
                static_cast<unsigned long long>(e.a), static_cast<unsigned long long>(e.b),
                static_cast<unsigned long long>(e.c >> 16), static_cast<unsigned long long>(e.c & 0xFFFF), e.thread);
            for (std::uint32_t f = 0; f < e.frames && n < static_cast<int>(sizeof(line)) - 40; ++f)
            {
                const auto rip = e.stack[f];
                if (rip >= g_exeBase && rip < g_exeBase + kRetailImageSize)
                    n += std::snprintf(line + n, sizeof(line) - n, " +%llX", static_cast<unsigned long long>(rip - g_exeBase));
                else
                    n += std::snprintf(line + n, sizeof(line) - n, " %llX", static_cast<unsigned long long>(rip));
            }
            Log("%s", line);
            ++logged;
        }
        g_traceLogged.store(logged, std::memory_order_relaxed);
    }
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto off = g_taskSkipOffPresent.load(std::memory_order_relaxed);
    static std::uint64_t lastSummary = 0;
    const bool stage = off && (present - off == 10 || present - off == 60 || present - off == 300);
    if (present - lastSummary < 300 && !stage)
        return;
    lastSummary = present;
    std::uint64_t n = 0, runs[2]{}, skips[2]{};
    for (auto& s : g_tasks)
    {
        if (!s.task.load(std::memory_order_acquire))
            continue;
        ++n;
        for (int ph = 0; ph < 2; ++ph)
        {
            runs[ph] += s.runs[ph].load(std::memory_order_relaxed);
            skips[ph] += s.skips[ph].load(std::memory_order_relaxed);
        }
    }
    char cbLine[900];
    int cn = std::snprintf(cbLine, sizeof(cbLine), "callbacks:");
    for (std::size_t i = 0; i < kCallbackSlots && cn < static_cast<int>(sizeof(cbLine)) - 30; ++i)
    {
        const auto cb = g_taskCallbacks[i].load(std::memory_order_relaxed);
        const auto hits = g_taskCallbackHits[i].load(std::memory_order_relaxed);
        if (!hits)
            continue;
        cn += std::snprintf(cbLine + cn, sizeof(cbLine) - cn, " +%llX:%llu",
            static_cast<unsigned long long>(cb), static_cast<unsigned long long>(hits));
    }
    Log("[TASKSKIP] %s present=%llu actorTasks=%llu runs=%llu/%llu skips=%llu/%llu overflow=%llu %s",
        off ? "after-off" : "stereo", static_cast<unsigned long long>(present), static_cast<unsigned long long>(n),
        static_cast<unsigned long long>(runs[0]), static_cast<unsigned long long>(runs[1]),
        static_cast<unsigned long long>(skips[0]), static_cast<unsigned long long>(skips[1]),
        static_cast<unsigned long long>(g_taskOverflow.load(std::memory_order_relaxed)), cbLine);
    if (!stage && (present % 900) != 0)
        return;
    std::uint32_t listed = 0;
    for (auto& s : g_tasks)
    {
        const auto task = s.task.load(std::memory_order_acquire);
        if (!task)
            continue;
        if (listed++ >= 80)
            break;
        char bufs[2][260];
        for (int ph = 0; ph < 2; ++ph)
            std::snprintf(bufs[ph], sizeof(bufs[ph]),
                "runs=%llu skips=%llu skipOnce=%llu runNow=%llu interval=%u maxInterval=%u counter=%u flags=%04X",
                static_cast<unsigned long long>(s.runs[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.skips[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.skipOnce[ph].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(s.runNow[ph].load(std::memory_order_relaxed)),
                s.interval[ph].load(std::memory_order_relaxed), s.maxInterval[ph].load(std::memory_order_relaxed),
                s.counter[ph].load(std::memory_order_relaxed), s.flags[ph].load(std::memory_order_relaxed));
        Log("[TASKSKIP]   task=%llX actor=%llX | STEREO %s | AFTER-OFF %s",
            static_cast<unsigned long long>(task), static_cast<unsigned long long>(s.actor.load(std::memory_order_relaxed)),
            bufs[0], bufs[1]);
    }
    if (off && (present - off == 60 || present - off >= 300))
        PopGateReport(present, off);
    if (off && present - off >= 300)
    {
        g_taskSkipArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &TaskSkipDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

// Present thread.
void AiModeTick()
{
    if (!g_aiModeArmed.load(std::memory_order_acquire))
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    // arm the pending-flag watch once the PlayMotion request has been seen
    static bool watchRequested = false;
    const auto pendingAddr = g_aiModePendingAddr.load(std::memory_order_acquire);
    if (pendingAddr && !watchRequested)
    {
        watchRequested = true;
        Log("[AIMODE] PlayMotion request seen: AI component=%llX (pending flag at %llX; no write watch this build)",
            static_cast<unsigned long long>(g_aiModeComponent.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(pendingAddr));
    }
    // drain
    const auto written = g_traceWritten.load(std::memory_order_acquire);
    auto logged = g_traceLogged.load(std::memory_order_relaxed);
    const auto off = g_aiModeOffPresent.load(std::memory_order_relaxed);
    while (logged < written)
    {
        const auto& e = g_trace[logged];
        char line[1500];
        int n;
        if (e.site == 0)
            n = std::snprintf(line, sizeof(line),
                "[AIMODE] request #%u present=%llu%s%lld comp=%llX mode=%08llX sub=%08llX%s tid=%u stack:",
                logged + 1, static_cast<unsigned long long>(e.present), off ? " off" : "",
                off ? static_cast<long long>(e.present) - static_cast<long long>(off) : 0LL,
                static_cast<unsigned long long>(e.a), static_cast<unsigned long long>(e.b),
                static_cast<unsigned long long>(e.c),
                (e.b & 0xFFFFFFFF) == kAiModePlayMotionId ? " <PLAYMOTION>" : "", e.thread);
        else
            n = std::snprintf(line, sizeof(line),
                "[AIMODE] TRAY-CTOR #%u present=%llu%s%lld tray=%llX rip=+%llX tid=%u stack:",
                logged + 1, static_cast<unsigned long long>(e.present), off ? " off" : "",
                off ? static_cast<long long>(e.present) - static_cast<long long>(off) : 0LL,
                static_cast<unsigned long long>(e.a),
                static_cast<unsigned long long>(e.b >= g_exeBase ? e.b - g_exeBase : e.b), e.thread);
        for (std::uint32_t f = 0; f < e.frames && n < static_cast<int>(sizeof(line)) - 40; ++f)
        {
            const auto rip = e.stack[f];
            if (rip >= g_exeBase && rip < g_exeBase + kRetailImageSize)
                n += std::snprintf(line + n, sizeof(line) - n, " +%llX",
                    static_cast<unsigned long long>(rip - g_exeBase));
            else
                n += std::snprintf(line + n, sizeof(line) - n, " %llX",
                    static_cast<unsigned long long>(rip));
        }
        Log("%s", line);
        ++logged;
    }
    g_traceLogged.store(logged, std::memory_order_relaxed);
    static std::uint64_t lastSummary = 0;
    if (present - lastSummary >= 300)
    {
        lastSummary = present;
        Log("[AIMODE] summary present=%llu requests=%llu trayCtors=%llu dropped=%llu",
            static_cast<unsigned long long>(present),
            static_cast<unsigned long long>(g_traceHits[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_traceHits[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_traceDropped.load(std::memory_order_relaxed)));
    }
    if (off && present - off >= 300)
    {
        g_aiModeArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &AiModeDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

std::uint32_t SetExecBreakpointAllThreads(int index, std::uintptr_t address, bool enable, bool onlyIfAvailable)
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto process = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();
    std::uint32_t applied = 0;
    const std::uint64_t controlMask = 0xFull << (16 + index * 4);
    const std::uint64_t enableMask = 0x3ull << (index * 2);
    const std::uint64_t localEnable = 0x1ull << (index * 2);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            const auto thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            DebugRegisterUpdates::Guard registerUpdate;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context))
                {
                    DWORD64* slot = index == 0 ? &context.Dr0 : index == 1 ? &context.Dr1 :
                        index == 2 ? &context.Dr2 : &context.Dr3;
                    const bool ownedOrAvailable = enable
                        ? !(context.Dr7 & enableMask) || *slot == address
                        : *slot == address;
                    if (!onlyIfAvailable || ownedOrAvailable)
                    {
                        context.Dr7 &= ~controlMask; // RW=00 execute, LEN=00
                        context.Dr7 &= ~enableMask;
                        if (enable)
                        {
                            *slot = address;
                            context.Dr7 |= localEnable;
                        }
                        else
                            *slot = 0;
                        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                        if (SetThreadContext(thread, &context))
                            ++applied;
                    }
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return applied;
}

void SetMotionGateProbe(bool enable)
{
    if (!g_exeBase)
        return;
    const auto a = SetExecBreakpointAllThreads(2, g_exeBase + kMotionGateARva, enable);
    const auto b = SetExecBreakpointAllThreads(3, g_exeBase + kMotionGateBRva, enable);
    g_motionGateArmed.store(enable && a && b, std::memory_order_release);
    if (enable)
        g_motionGateArmPresent.store(g_presentCount.load(std::memory_order_relaxed),
            std::memory_order_release);
    Log("[MOTIONGATE] probe %s: gate A +0x%llX on %u threads, gate B +0x%llX on %u threads",
        enable ? "ARMED" : "disarmed", static_cast<unsigned long long>(kMotionGateARva), a,
        static_cast<unsigned long long>(kMotionGateBRva), b);
}

DWORD WINAPI MotionGateArmThread(LPVOID)
{
    SetMotionGateProbe(true);
    return 0;
}

DWORD WINAPI MotionGateDisarmThread(LPVOID)
{
    SetMotionGateProbe(false);
    return 0;
}

// Exception handler side.  gate: 0 = A, 1 = B.
void MotionGateRecord(int gate, const CONTEXT* trap)
{
    const bool nonzero = (trap->Rax & 0xFF) != 0;
    g_motionGateHits[gate][nonzero ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    const auto request = static_cast<std::uintptr_t>(trap->R12);
    if (!request)
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    for (auto& slot : g_motionReqs)
    {
        auto existing = slot.request.load(std::memory_order_acquire);
        if (existing != request)
        {
            if (existing || !slot.request.compare_exchange_strong(existing, request,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                if (existing != request)
                    continue;
            }
            else
                slot.firstPresent.store(present, std::memory_order_relaxed);
        }
        slot.hits[gate][nonzero ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
        slot.lastPresent.store(present, std::memory_order_relaxed);
        slot.lastAl.store((static_cast<std::uint64_t>(gate) << 8) | (trap->Rax & 0xFF),
            std::memory_order_relaxed);
        __try
        {
            slot.handleQ0.store(*reinterpret_cast<const std::uint64_t*>(request),
                std::memory_order_relaxed);
            slot.explicitId8.store(*reinterpret_cast<const std::uint32_t*>(request + 8),
                std::memory_order_relaxed);
            slot.resourceC.store(*reinterpret_cast<const std::uint32_t*>(request + 0xC),
                std::memory_order_relaxed);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return;
    }
    g_motionReqOverflow.fetch_add(1, std::memory_order_relaxed);
}

// Present thread.
void MotionGateTick()
{
    if (!g_motionGateArmed.load(std::memory_order_acquire))
        return;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto off = g_motionGateOffPresent.load(std::memory_order_relaxed);
    static std::uint64_t lastReport = 0;
    const bool releaseStage = off && (present - off == 10 || present - off == 60 ||
        present - off == 150 || present - off == 300);
    if (present - lastReport < 150 && !releaseStage)
        return;
    lastReport = present;
    const char* phase = !off ? "stereo" : "after-off";
    Log("[MOTIONGATE] %s present=%llu%s%lld totals: A yes/no=%llu/%llu  B ready/notready=%llu/%llu  overflow=%llu",
        phase, static_cast<unsigned long long>(present), off ? " off" : "",
        off ? static_cast<long long>(present) - static_cast<long long>(off) : 0LL,
        static_cast<unsigned long long>(g_motionGateHits[0][1].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_motionGateHits[0][0].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_motionGateHits[1][1].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_motionGateHits[1][0].load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_motionReqOverflow.load(std::memory_order_relaxed)));
    std::uint32_t listed = 0;
    for (auto& slot : g_motionReqs)
    {
        const auto request = slot.request.load(std::memory_order_acquire);
        if (!request)
            continue;
        const auto last = slot.lastPresent.load(std::memory_order_relaxed);
        if (present - last > 300)
            continue;
        if (listed++ >= 24)
            break;
        Log("[MOTIONGATE]   req=%llX first=%llu last=%llu | A yes=%llu no=%llu | B ready=%llu notready=%llu | "
            "handle=%llX id8=%08X resC=%08X lastGate=%c al=%llu",
            static_cast<unsigned long long>(request),
            static_cast<unsigned long long>(slot.firstPresent.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(last),
            static_cast<unsigned long long>(slot.hits[0][1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(slot.hits[0][0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(slot.hits[1][1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(slot.hits[1][0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(slot.handleQ0.load(std::memory_order_relaxed)),
            slot.explicitId8.load(std::memory_order_relaxed),
            slot.resourceC.load(std::memory_order_relaxed),
            (slot.lastAl.load(std::memory_order_relaxed) >> 8) ? 'B' : 'A',
            static_cast<unsigned long long>(slot.lastAl.load(std::memory_order_relaxed) & 0xFF));
    }
    if (off && present - off >= 300)
    {
        g_motionGateArmed.store(false, std::memory_order_release);
        if (const auto worker = CreateThread(nullptr, 0, &MotionGateDisarmThread, nullptr, 0, nullptr))
            CloseHandle(worker);
    }
}

void RecordDynamicHeadWriter(std::uintptr_t rip)
{
    if (rip < g_exeBase || rip >= g_exeBase + kRetailImageSize)
        return;
    for (auto& slot : g_dynamicHeadWriters)
    {
        auto existing = slot.rip.load(std::memory_order_acquire);
        if (existing == rip)
        {
            slot.hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!existing && slot.rip.compare_exchange_strong(existing, rip,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            slot.hits.store(1, std::memory_order_release);
            return;
        }
    }
}

LONG CALLBACK DynamicHeadHandlerInner(EXCEPTION_POINTERS* info);

// [ORPHANTRAP] A crash: a
// single-step at the head-tracking writer +0x921424E with Dr0 == Rip, Dr7
// still arming it, B0 set - raised on one thread just before stereo-off's
// disarm reached it, handled after the worker had flipped the phase to Off,
// so the inner handler disowned its own breakpoint and the process died.
// Same family as the census crash (Rip == Dr2 with Dr6 == 0).  Rule: a
// debug trap produced by one of the mod's debug registers is always resumed,
// whatever state the feature that armed it is in by the time it is seen.
// The trap's saved Dr7 says which register was live when it fired:
//   execute breakpoint (RW = 00) at Rip -> set RF and continue;
//   data breakpoint status bit set     -> the write already happened, continue.
LONG CALLBACK DynamicHeadHandler(EXCEPTION_POINTERS* info)
{
    const LONG inner = DynamicHeadHandlerInner(info);
    if (inner != EXCEPTION_CONTINUE_SEARCH || !info || !info->ExceptionRecord ||
        !info->ContextRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return inner;
    auto* context = info->ContextRecord;
    const auto rip = static_cast<std::uintptr_t>(context->Rip);
    const std::uint64_t dr[4] = {context->Dr0, context->Dr1, context->Dr2, context->Dr3};
    static std::atomic_uint32_t orphanLogs{};
    for (int i = 0; i < 4; ++i)
    {
        const bool enabled = (context->Dr7 >> (i * 2)) & 0x3;
        const auto rw = (context->Dr7 >> (16 + i * 4)) & 0x3;
        const bool fired = (context->Dr6 >> i) & 0x1;
        if (!enabled || !dr[i])
            continue;
        if (rw == 0 && dr[i] == rip)
        {
            if (orphanLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("[ORPHANTRAP] resumed orphaned execute breakpoint DR%d at +0x%llX (Dr6=%llX)",
                    i, static_cast<unsigned long long>(g_exeBase && rip >= g_exeBase ? rip - g_exeBase : rip),
                    static_cast<unsigned long long>(context->Dr6));
            context->Dr6 = 0;
            context->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (rw != 0 && fired)
        {
            if (orphanLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("[ORPHANTRAP] resumed orphaned data breakpoint DR%d addr=%llX rip=+0x%llX",
                    i, static_cast<unsigned long long>(dr[i]),
                    static_cast<unsigned long long>(g_exeBase && rip >= g_exeBase ? rip - g_exeBase : rip));
            context->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    // Dr6 reported nothing (it is unreliable) but one of the mod's DATA breakpoints
    // is enabled on this thread: nothing else single-steps this process, so
    // the trap is the mod's.
    for (int i = 0; i < 4; ++i)
    {
        const bool enabled = (context->Dr7 >> (i * 2)) & 0x3;
        const auto rw = (context->Dr7 >> (16 + i * 4)) & 0x3;
        if (enabled && dr[i] && rw != 0)
        {
            if (orphanLogs.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("[ORPHANTRAP] resumed unattributed single-step with DR%d data watch enabled "
                    "addr=%llX rip=+0x%llX Dr6=%llX",
                    i, static_cast<unsigned long long>(dr[i]),
                    static_cast<unsigned long long>(g_exeBase && rip >= g_exeBase ? rip - g_exeBase : rip),
                    static_cast<unsigned long long>(context->Dr6));
            context->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return inner;
}

LONG CALLBACK DynamicHeadHandlerInner(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord || !info->ContextRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    auto* context = info->ContextRecord;
    // Execute breakpoints are matched on the INSTRUCTION ADDRESS, not on the
    // Dr6 status bit.  Crash dumps
    // show a single-step exception with Rip == Dr2 and Dr6 == 0: Windows does
    // not always report the B-bit, so gating on it passed the mod's own breakpoint
    // on as an unhandled exception.  Dr6 remains the fallback for the data
    // breakpoint on DR0.
    const auto hitRip = static_cast<std::uintptr_t>(context->Rip);
    const auto observerSite = g_cameraObserverSite.load(std::memory_order_acquire);
    if (observerSite && hitRip == observerSite && context->Dr2 == observerSite &&
        (context->Dr7 & 0x30ull))
    {
        const auto current = static_cast<std::uintptr_t>(context->Rax);
        g_cameraObserverLastSource.store(current, std::memory_order_release);
        g_cameraObserverHits.fetch_add(1, std::memory_order_relaxed);
        if (current != g_dynamicHeadSource.load(std::memory_order_acquire))
            g_cameraObserverMismatches.fetch_add(1, std::memory_order_relaxed);
        CaptureRenderMotion(9, reinterpret_cast<const float*>(current));
        if (g_dynamicHeadPhase.load(std::memory_order_acquire) == DynamicHeadPhase::Active)
        {
            float yaw{}, pitch{}, roll{}, right{}, up{}, forward{};
            std::uint64_t serial{};
            const bool poseAvailable = RetailXr::GetHeadPose(&yaw, &pitch, &roll,
                &right, &up, &forward, &serial);
            alignas(16) float observed[16]{};
            bool copied=false;
            __try
            {
                if (IsDynamicHeadMatrix(current))
                {
                    std::memcpy(observed, reinterpret_cast<const void*>(current), sizeof(observed));
                    copied=true;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (TryAcquireSRWLockExclusive(&g_headRecoveryLock))
            {
                g_headRecoveryHealth.Observe(current, copied ? observed : nullptr,
                    GetTickCount64(), poseAvailable);
                ReleaseSRWLockExclusive(&g_headRecoveryLock);
            }
        }
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    const bool atFov = g_exeBase && hitRip == g_exeBase + kFovSetterRva;
    const bool atCensus = [&] {
        const auto t = g_seqCensusCallSite.load(std::memory_order_relaxed);
        return t && hitRip == t;
    }();
    const bool atIseq = [&] {
        const auto t = g_iseqExecuteAddress.load(std::memory_order_relaxed);
        return t && hitRip == t;
    }();
    if (g_exeBase && (hitRip == g_exeBase + kPopGateWrapRva || hitRip == g_exeBase + kPopGateLoopRva))
    {
        PopGateRecord(context, hitRip == g_exeBase + kPopGateLoopRva);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && hitRip == g_exeBase + kTaskInvokerRva)
    {
        TaskHopRecord(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && hitRip == g_exeBase + kActorUpdateJobRva)
    {
        ActRateRecord(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && hitRip == g_exeBase + kTaskSkipRuleRva)
    {
        TaskSkipRecord(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && (hitRip == g_exeBase + kModeSwReqRva || hitRip == g_exeBase + kModeSwTrayRva))
    {
        if (hitRip == g_exeBase + kModeSwReqRva)
            ModeSwRecordReq(context);
        else
            ModeSwRecordTray(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && (hitRip == g_exeBase + kCanApplyAnswerRva || hitRip == g_exeBase + kCanApplyWaitRva))
    {
        if (hitRip == g_exeBase + kCanApplyAnswerRva)
            ApplyRecordAnswer(context);
        else
            ApplyRecordWait(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && (hitRip == g_exeBase + kAiCompStateRva || hitRip == g_exeBase + kAiCompGateRva))
    {
        if (hitRip == g_exeBase + kAiCompStateRva)
            CompRecordState(context);
        else
            CompRecordGate(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && (hitRip == g_exeBase + kActUpdFlagRva || hitRip == g_exeBase + kActUpdCtlRva))
    {
        if (hitRip == g_exeBase + kActUpdFlagRva)
            ActRecordFlags(context);
        else
            ActRecordCtl(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && (hitRip == g_exeBase + kTrayAiModeUpdateRva || hitRip == g_exeBase + kTrayMindTaskUpdateRva))
    {
        TrayRecord(hitRip == g_exeBase + kTrayAiModeUpdateRva ? 0 : 1, context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && hitRip == g_exeBase + kAiModeRequestRva)
    {
        AiModeRecordRequest(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && hitRip == g_exeBase + kAiModeTrayCtorRva)
    {
        TraceCapture(1, context, context->Rcx, context->Rip, 0);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && (hitRip == g_exeBase + kAiGraphCondRva || hitRip == g_exeBase + kAiGraphNodeRva))
    {
        if (hitRip == g_exeBase + kAiGraphCondRva)
            AiGraphRecordCond(context);
        else
            AiGraphRecordNode(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && hitRip == g_exeBase + kAiGraphCondEntryRva)
    {
        AiGraphRecordCondEntry(context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_exeBase && (hitRip == g_exeBase + kMotionGateARva || hitRip == g_exeBase + kMotionGateBRva))
    {
        MotionGateRecord(hitRip == g_exeBase + kMotionGateARva ? 0 : 1, context);
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // [FOVSET] DR1: the engine FOV setter.  Override the incoming value
    // (XMM1 low float, radians) and capture the true manager pointer.
    if ((context->Dr6 & 0x2) || atFov)
    {
        if (static_cast<std::uintptr_t>(context->Rip) ==
            g_exeBase + kFovSetterRva)
        {
            g_fovSetManager.store(static_cast<std::uintptr_t>(context->Rcx),
                std::memory_order_relaxed);
            const auto hits =
                g_fovSetHits.fetch_add(1, std::memory_order_relaxed) + 1;
            {
                const auto channel = static_cast<std::uint32_t>(context->R8 & 0xFFFFFFFF);
                g_fovLastChannel.store(channel, std::memory_order_relaxed);
                g_fovLastHitPresent.store(g_presentCount.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                (channel ? g_fovNonZeroHits : g_fovZeroHits).fetch_add(1,
                    std::memory_order_relaxed);
            }
            float requested = 0.0f;
            std::memcpy(&requested, &context->Xmm1, sizeof(requested));
            float replaced = requested;
            if (VrCameraEnabled() ||
                g_camParamAllModes.load(std::memory_order_relaxed))
            {
                const float degrees =
                    g_liveFovDegrees.load(std::memory_order_relaxed);
                if (degrees > 10.0f && degrees < 179.0f &&
                    requested > 0.05f && requested < 3.1f)
                {
                    const float safe = degrees > 87.0f ? 87.0f : degrees;
                    replaced = safe * 3.14159265358979323846f / 180.0f;
                    std::memcpy(&context->Xmm1, &replaced, sizeof(replaced));
                    // Setter +91923D0: arg5=[entry rsp+28] duration,
                    // arg7=[entry rsp+38] explicit start lens (-1=current).
                    // Native +918AA70 writes that start into channel+14.
                    // Replacing only the target lets an authored narrow lens
                    // flash before the override is reached again.
                    if (g_fovPinTransitionStart.load(std::memory_order_acquire) &&
                        static_cast<std::uint32_t>(context->R8) < kFovChanCount)
                    {
                        __try
                        {
                            const float duration = *reinterpret_cast<const float*>(context->Rsp + 0x28);
                            auto* start = reinterpret_cast<float*>(context->Rsp + 0x38);
                            const float before = *start;
                            if (ShouldPinFovTransitionStart(duration,
                                    static_cast<std::uint32_t>(context->R9), before))
                            {
                                *start = replaced;
                                const auto pinned = g_fovTransitionStartPinned.fetch_add(1,
                                    std::memory_order_relaxed) + 1;
                                if (pinned <= 6)
                                    Log("[FOVSTART] channel=%u mode=%u duration=%.4f start=%.4f -> %.4f rad; native completion retained",
                                        static_cast<std::uint32_t>(context->R8),
                                        static_cast<std::uint32_t>(context->R9), duration, before, replaced);
                            }
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }

                }
            }
            if (hits <= 6 || (hits % 900) == 0)
                Log("[FOVSET] hit=%llu mgr=%llX channel=%u %.4f -> %.4f rad",
                    static_cast<unsigned long long>(hits),
                    static_cast<unsigned long long>(context->Rcx),
                    static_cast<std::uint32_t>(context->R8 & 0xFFFFFFFF),
                    requested, replaced);
            context->Dr6 = 0;
            context->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        context->Dr6 &= ~0x2ull;
    }
    // [SEQCENSUS] DR2: the sequence tick's execute call site; rcx = action.
    if ((context->Dr6 & 0x4) || atCensus)
    {
        const auto target = g_seqCensusCallSite.load(std::memory_order_relaxed);
        if (target && static_cast<std::uintptr_t>(context->Rip) == target)
        {
            const auto self = static_cast<std::uintptr_t>(context->Rcx);
            std::uintptr_t vtable = 0;
            __try
            {
                if (self && IsReadable(reinterpret_cast<const void*>(self), 8))
                    vtable = *reinterpret_cast<const std::uintptr_t*>(self);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                vtable = 0;
            }
            if (g_seqCensusArmed.load(std::memory_order_relaxed))
            {
                g_seqCensusHits.fetch_add(1, std::memory_order_relaxed);
                if (vtable >= g_exeBase && vtable < g_exeBase + kRetailImageSize)
                {
                    const auto present = g_presentCount.load(std::memory_order_relaxed);
                    SeqCensusRecord(vtable - g_exeBase, present);
                    SeqInstRecord(self, vtable - g_exeBase, present);
                    if (vtable - g_exeBase == kTimeLineBlackVtableRva)
                        TimelineRecord(self, present);
                    else if (vtable - g_exeBase == kExecAIModeWaitVtableRva ||
                        vtable - g_exeBase == kExecAIModePlayMotionVtableRva)
                        AiWaitRecord(self, vtable - g_exeBase, present);
                }
            }
            context->Dr6 = 0;
            context->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        context->Dr6 &= ~0x4ull;
    }
    // [CHARWATCH] DR3 data write watch on the character.  Data breakpoints
    // trap after the write: record and continue, no RF needed.
    if ((context->Dr6 & 0x8) && context->Dr3 &&
        context->Dr3 == g_charWatchAddress.load(std::memory_order_relaxed) &&
        ((context->Dr7 >> 28) & 0x3) == 0x1)
    {
        CharWatchRecord(context);
        context->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // [ISEQ] DR3: SequenceActionInteraction::execute entry.  Observe only.
    if ((context->Dr6 & 0x8) || atIseq)
    {
        const auto target = g_iseqExecuteAddress.load(std::memory_order_relaxed);
        if (target && static_cast<std::uintptr_t>(context->Rip) == target)
        {
            const auto self = static_cast<std::uintptr_t>(context->Rcx);
            bool match = false;
            __try
            {
                match = self != 0 && IsReadable(reinterpret_cast<const void*>(self), 8) &&
                    *reinterpret_cast<const std::uintptr_t*>(self) ==
                        g_exeBase + kSeqActionInteractionVtableRva;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                match = false;
            }
            if (match)
            {
                g_iseqPolls.fetch_add(1, std::memory_order_relaxed);
                g_iseqLastPollPresent.store(g_presentCount.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                g_iseqLastSelf.store(self, std::memory_order_relaxed);
                __try
                {
                    if (IsReadable(reinterpret_cast<const void*>(self), kIseqSnapshotBytes))
                    {
                        std::memcpy(g_iseqSnapshot, reinterpret_cast<const void*>(self),
                            kIseqSnapshotBytes);
                        g_iseqSnapshotSeq.fetch_add(1, std::memory_order_release);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            else
                g_iseqOtherVtableHits.fetch_add(1, std::memory_order_relaxed);
            context->Dr6 = 0;
            context->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        context->Dr6 &= ~0x8ull;
    }
    if (!(context->Dr6 & 0x1))
        return EXCEPTION_CONTINUE_SEARCH;

    const auto phase = g_dynamicHeadPhase.load(std::memory_order_acquire);
    const auto rip = static_cast<std::uintptr_t>(context->Rip);
    if (phase == DynamicHeadPhase::CaptureSource &&
        rip == g_dynamicHeadCaptureSite.load(std::memory_order_acquire))
    {
        const auto source = static_cast<std::uintptr_t>(context->Rax);
        if (IsDynamicHeadMatrix(source))
        {
            std::uintptr_t expected = 0;
            g_dynamicHeadSource.compare_exchange_strong(expected, source,
                std::memory_order_acq_rel, std::memory_order_acquire);
            g_dynamicHeadCaptureHits.fetch_add(1, std::memory_order_relaxed);
        }
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (phase == DynamicHeadPhase::WatchWriter)
    {
        RecordDynamicHeadWriter(rip);
        const auto hits = g_dynamicHeadWriterWatchHits.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (hits >= 200)
            context->Dr7 &= ~0x1ull;
        context->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (phase == DynamicHeadPhase::Active &&
        rip == g_dynamicHeadWriter.load(std::memory_order_acquire))
    {
        // For the validated shared writer, record its actual destination
        // before the unchanged tracking code writes its cached source.
        if (g_cameraObserverSite.load(std::memory_order_relaxed) &&
            rip == g_exeBase + 0x9215560)
            CaptureRenderMotion(10, reinterpret_cast<const float*>(context->Rsi + 0xA0));
        g_headTrackCalls.fetch_add(1, std::memory_order_relaxed);
        bool applied = false;
        float headYaw = 0.0f;
        float headPitch = 0.0f;
        float headRoll = 0.0f;
        float headRight = 0.0f;
        float headUp = 0.0f;
        float headForward = 0.0f;
        std::uint64_t headSerial = 0;
        const auto source = g_dynamicHeadSource.load(std::memory_order_acquire);
        if (VrCameraEnabled() &&
            RetailXr::GetHeadPose(&headYaw, &headPitch, &headRoll,
                &headRight, &headUp, &headForward, &headSerial) &&
            IsDynamicHeadMatrix(source) &&
            IsWritable(reinterpret_cast<void*>(source), sizeof(float) * 16))
        {
            __try
            {
                alignas(16) float matrix[16]{};
                std::memcpy(matrix, reinterpret_cast<const void*>(source), sizeof(matrix));
                // Census the game's own value, before any modification.
                RecordViewCensus(source, matrix);
                if (g_stereoEnabled.load(std::memory_order_relaxed))
                {
                    float viewEye[3]{};
                    ViewEyePosition(matrix, viewEye);
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        g_headGameRow3[axis].store(matrix[12 + axis],
                            std::memory_order_relaxed);
                        g_headGameViewEye[axis].store(viewEye[axis],
                            std::memory_order_relaxed);
                    }
                }
                // [AERSEQ] This build owns its eye - the parity of its own
                // sequence number - instead of reading a "next eye" the
                // presenter arms from another thread.  That arm was a feedback
                // loop: the build happens here, the pixels reach Present one
                // or more frames later, and whenever the depth between the two
                // changed the label stopped matching the pixels.  Peek now,
                // publish once the camera write has actually landed, so a
                // build that fails never claims a frame.
                const bool aerActiveBuild =
                    g_aerEnabled.load(std::memory_order_acquire);
                const int aerBuildEye =
                    aerActiveBuild ? RetailXr::PeekAerBuildEye() : 0;
                // [EYEHOLD] Give both eyes of a pair the same game camera, so
                // the only difference between them is the IPD offset.
                if (g_aerEnabled.load(std::memory_order_acquire) &&
                    g_eyeHoldEnabled.load(std::memory_order_acquire) &&
                    !RetailXr::GetMonoSubmit())
                {
                    const int holdEye = aerBuildEye;
                    const auto present =
                        g_presentCount.load(std::memory_order_relaxed);
                    if (holdEye == 0)
                    {
                        g_eyeHoldAppliedThisFrame.store(false,
                            std::memory_order_relaxed);
                        {
                            // [AERSYNC] how far the game camera travelled since
                            // the previous pair.
                            float now[3]{}, before[3]{};
                            ViewEyePosition(matrix, now);
                            AcquireSRWLockShared(&g_eyeHoldLock);
                            ViewEyePosition(g_eyeHoldMatrix, before);
                            ReleaseSRWLockShared(&g_eyeHoldLock);
                            const float d = std::sqrt((now[0] - before[0]) * (now[0] - before[0]) +
                                (now[1] - before[1]) * (now[1] - before[1]) +
                                (now[2] - before[2]) * (now[2] - before[2]));
                            if (std::isfinite(d) && d < 50.0f &&
                                g_eyeHoldValid.load(std::memory_order_acquire))
                            {
                                g_aerPairMoveUm.fetch_add(static_cast<std::uint64_t>(d * 1e6f),
                                    std::memory_order_relaxed);
                                g_aerPairSamples.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        AcquireSRWLockExclusive(&g_eyeHoldLock);
                        std::memcpy(g_eyeHoldMatrix, matrix, sizeof(matrix));
                        g_eyeHoldPose.yaw = headYaw;
                        g_eyeHoldPose.pitch = headPitch;
                        g_eyeHoldPose.roll = headRoll;
                        g_eyeHoldPose.right = headRight;
                        g_eyeHoldPose.up = headUp;
                        g_eyeHoldPose.forward = headForward;
                        g_eyeHoldPose.serial = headSerial;
                        ReleaseSRWLockExclusive(&g_eyeHoldLock);
                        g_eyeHoldPresent.store(present, std::memory_order_release);
                        g_eyeHoldValid.store(true, std::memory_order_release);
                        g_eyeHoldCaptured.fetch_add(1, std::memory_order_relaxed);
                    }
                    else if (g_eyeHoldValid.load(std::memory_order_acquire))
                    {
                        // Only reuse a capture from the immediately preceding
                        // frames; a stale one would freeze the camera outright.
                        const auto captured =
                            g_eyeHoldPresent.load(std::memory_order_acquire);
                        if (present - captured <= 3)
                        {
                            AcquireSRWLockShared(&g_eyeHoldLock);
                            {
                                // [AERSYNC] how far the game's own camera is from
                                // the first eye's, before the hold replaces it.
                                float fresh[3]{}, held[3]{};
                                ViewEyePosition(matrix, fresh);
                                ViewEyePosition(g_eyeHoldMatrix, held);
                                const float d = std::sqrt((fresh[0] - held[0]) * (fresh[0] - held[0]) +
                                    (fresh[1] - held[1]) * (fresh[1] - held[1]) +
                                    (fresh[2] - held[2]) * (fresh[2] - held[2]));
                                if (std::isfinite(d) && d < 50.0f)
                                    g_aerPairGapUm.fetch_add(static_cast<std::uint64_t>(d * 1e6f),
                                        std::memory_order_relaxed);
                            }
                            std::memcpy(matrix, g_eyeHoldMatrix, sizeof(matrix));
                            // A complete AER pair must share the head pose as
                            // well as the game camera.  A pair
                            // capture proved that a fresh second-eye pose put
                            // per-frame motion back into most pairs.  OpenXR
                            // reprojects the completed pair from this rendered
                            // pose, preserving smooth headset tracking.
                            if (!g_aerFreshHeadPose.load(std::memory_order_relaxed))
                            {
                                headYaw = g_eyeHoldPose.yaw;
                                headPitch = g_eyeHoldPose.pitch;
                                headRoll = g_eyeHoldPose.roll;
                                headRight = g_eyeHoldPose.right;
                                headUp = g_eyeHoldPose.up;
                                headForward = g_eyeHoldPose.forward;
                                headSerial = g_eyeHoldPose.serial;
                            }
                            ReleaseSRWLockShared(&g_eyeHoldLock);
                            g_eyeHoldAppliedThisFrame.store(true,
                                std::memory_order_relaxed);
                            const auto applied = g_eyeHoldApplied.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                            if (applied <= 3 || (applied % 3600) == 0)
                                Log("[EYEHOLD] pair held applied=%llu captured=%llu "
                                    "stale=%llu",
                                    static_cast<unsigned long long>(applied),
                                    static_cast<unsigned long long>(
                                        g_eyeHoldCaptured.load(
                                            std::memory_order_relaxed)),
                                    static_cast<unsigned long long>(
                                        g_eyeHoldStale.load(
                                            std::memory_order_relaxed)));
                        }
                        else
                        {
                            g_eyeHoldStale.fetch_add(1, std::memory_order_relaxed);
                            g_eyeHoldValid.store(false, std::memory_order_release);
                        }
                    }
                }
                if (g_decoupledPitch.load(std::memory_order_relaxed) &&
                    g_conversationActive.load(std::memory_order_relaxed) == 0)
                    LevelCameraPitch(matrix);   // [DECOUPLEPITCH]
                const bool customFirstPerson =
                    g_customFirstPersonEnabled.load(std::memory_order_acquire);
                const bool sixDof =
                    g_sixDofEnabled.load(std::memory_order_acquire);
                const float positionScale = sixDof
                    ? g_sixDofPositionScale.load(std::memory_order_acquire) : 0.0f;
                const float rightOffset =
                    (customFirstPerson
                        ? g_customFirstPersonRight.load(std::memory_order_acquire)
                        : 0.0f) + headRight * positionScale;
                const float upOffset =
                    (customFirstPerson
                        ? g_customFirstPersonUp.load(std::memory_order_acquire)
                        : 0.0f) + headUp * positionScale;
                const float forwardOffset =
                    (customFirstPerson
                        ? g_customFirstPersonForward.load(std::memory_order_acquire)
                        : 0.0f) - headForward * positionScale;
                if ((customFirstPerson || sixDof) &&
                    ApplyCameraLocalOffset(
                        matrix, rightOffset, upOffset, forwardOffset))
                {
                    const auto positionCount = g_sixDofPositionApplied.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                    if (positionCount <= 4)
                        Log("[6DOF] pose local=%.4f/%.4f/%.4f m roll=%.4f scale=%.2f custom=%d",
                            headRight, headUp, headForward, headRoll,
                            positionScale, customFirstPerson ? 1 : 0);
                }
                const float uicamBase[3]{matrix[2], matrix[6], matrix[10]};
                {
                    // [WORLDLOCK] the game's own camera before the head is
                    // applied: with the stick idle it should not jitter when
                    // only the head moves.
                    static WorldLock::Series base;
                    static int baseMode = -1;
                    static float lastYaw = 0.0f, lastPitch = 0.0f;
                    const int mode = aerActiveBuild ? (RetailXr::GetMonoSubmit() ? 2 : 1) : 0;
                    if (mode != baseMode) { base.Reset(); baseMode = mode; }
                    float r[9]{};
                    WorldLock::ViewRotation(matrix, r);
                    const bool moving = std::fabs(headYaw - lastYaw) + std::fabs(headPitch - lastPitch) > 0.0017f;
                    lastYaw = headYaw; lastPitch = headPitch;
                    base.Add(r, moving);
                    if (base.n >= 600)
                    {
                        static const char* names[] = {"Stereo", "AER", "Mono"};
                        Log("[BASELOCK] %s game camera before head over %llu writes: jitter rms %.3f deg (head moving %.3f, %llu writes) max %.3f, drift %.3f deg/write",
                            names[mode], static_cast<unsigned long long>(base.n), base.Rms(), base.RmsMoving(),
                            static_cast<unsigned long long>(base.moving), base.jitterMax, base.MeanStep());
                        base.n = base.moving = 0; base.step = base.jitter2 = base.jitterMax = base.jitterMoving2 = 0.0;
                    }
                }
                if (ApplyHeadRotation(matrix, -headYaw, headPitch, 0.0f))
                {
                    {
                        const float uicamHead[3]{matrix[2], matrix[6], matrix[10]};
                        UiHook::SetCameraAxes(uicamBase, uicamHead);
                    }
                    const bool aerActive = aerActiveBuild;
                    int probeEye = 0;
                    if (aerActive)
                    {
                        // [AFWPIN] AFW renders one eye every frame and
                        // synthesizes the other, so the offset is pinned
                        // instead of alternating.
                        const int eye = g_afwEnabled.load(std::memory_order_acquire)
                            ? 0 : aerBuildEye;
                        probeEye = eye;
                        float sign = eye == 0 ? -1.0f : 1.0f;
                        if (g_aerSwapEyes.load(std::memory_order_acquire))
                            sign = -sign;
                        const float halfEye = RetailXr::GetMonoSubmit() ? 0.0f   // [MONOVR]
                            : g_aerHalfEyeMeters.load(std::memory_order_acquire);
                        if (halfEye > 0.0f && ApplyAerEyeOffset(matrix, sign * halfEye))
                            g_aerEyeOffsetApplied.fetch_add(1,
                                std::memory_order_relaxed);
                    }
                    else if (g_stereoEnabled.load(std::memory_order_acquire) &&
                        !g_stereoSuspended.load(std::memory_order_relaxed) &&
                        g_nativeSymmetric.load(std::memory_order_relaxed) &&
                        g_stereoBaselineAddress)
                    {
                        // [SPLITSYM] Left eye = centre - b/2.  The right-eye
                        // seat (ApplyCamSourceSway) adds the full baseline on
                        // top of this camera, landing at centre + b/2.
                        // Baseline is engine centimetres; the world is metres.
                        const float baseline = *g_stereoBaselineAddress;
                        const float half = baseline * 0.01f * 0.5f;
                        const float leftSign = RetailXr::GetSplitSwapEyes() ? 1.0f : -1.0f;   // [MESWAP]
                        if (half > 1e-5f && half < 0.5f &&
                            ApplyAerEyeOffset(matrix, leftSign * half))
                        {
                            const auto count = g_nativeLeftOffsetApplied.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                            if (count <= 3 || (count % 3600) == 0)
                                Log("[SPLITSYM] left eye -%.4f m applied=%llu "
                                    "(right-eye seat adds +%.4f m)",
                                    half, static_cast<unsigned long long>(count),
                                    baseline * 0.01f);
                        }
                    }
                    // Measured on the committed matrix, so the numbers are the
                    // ones actually rendered this frame.
                    ProbeModeState(source, matrix, probeEye, aerActive);
                    if (!aerActive && g_stereoEnabled.load(std::memory_order_relaxed))
                    {
                        float committedViewEye[3]{};
                        ViewEyePosition(matrix, committedViewEye);
                        {
                            // columns: right=(m0,m4,m8) up=(m1,m5,m9) back=(m2,m6,m10)
                            const int columns[9] = {0, 4, 8, 1, 5, 9, 2, 6, 10};
                            for (int cell = 0; cell < 9; ++cell)
                                g_splitLeftAxes[cell].store(matrix[columns[cell]],
                                    std::memory_order_relaxed);
                        }
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            g_splitLeftEye[axis].store(matrix[12 + axis],
                                std::memory_order_relaxed);
                            g_splitLeftViewEye[axis].store(committedViewEye[axis],
                                std::memory_order_relaxed);
                        }
                        g_splitLeftSamples.fetch_add(1, std::memory_order_relaxed);
                    }
                    std::memcpy(reinterpret_cast<void*>(source), matrix, sizeof(matrix));
                    // [RENDERCAM] every mode, not just stereo: this is what lets
                    // the camera the GPU actually rendered be matched back to
                    // the head sample that produced it.
                    RetailXr::RecordHeadCameraPose(headSerial, matrix, aerActive ? probeEye : -1);
                    PushCommittedCamera(matrix);   // [MONOCAM]
                    if (!aerActive)
                    {
                        if (TryAcquireSRWLockExclusive(&g_headRecoveryLock))
                        {
                            g_headRecoveryHealth.Commit(source, matrix, GetTickCount64());
                            ReleaseSRWLockExclusive(&g_headRecoveryLock);
                        }
                        CaptureRenderMotion(1, matrix);
                    }
                    applied = true;
                    // [AERSEQ] The offset camera is committed, so this build
                    // will produce pixels: queue {seq, eye} for the present
                    // that shows them.
                    if (aerActive)
                        RetailXr::PublishAerBuildEye(headSerial, matrix);   // [AERTAG]
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                applied = false;
            }
        }
        if (applied)
        {
            RetailXr::MarkHeadPoseRendered(headSerial);
            g_headTrackApplied.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            g_headTrackRejected.fetch_add(1, std::memory_order_relaxed);
        }
        context->Dr6 = 0;
        context->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void ResetHeadRecoveryHealth()
{
    AcquireSRWLockExclusive(&g_headRecoveryLock);
    g_headRecoveryHealth.Reset();
    ReleaseSRWLockExclusive(&g_headRecoveryLock);
}

bool RecoverDynamicHeadWriter(std::uintptr_t source, std::uintptr_t& writer)
{
    if (!IsDynamicHeadMatrix(source)) return false;
    const auto oldSource=g_dynamicHeadSource.load(std::memory_order_acquire);
    const auto oldWriter=writer;
    const auto attempt=g_headRecoveryAttempts.fetch_add(1, std::memory_order_relaxed)+1;
    Log("[HEADRECOVER] attempt=%llu source=%p -> %p oldWriter=+0x%llX; observing actual camera writes",
        static_cast<unsigned long long>(attempt), reinterpret_cast<void*>(oldSource),
        reinterpret_cast<void*>(source), static_cast<unsigned long long>(oldWriter-g_exeBase));
    // This worker is the only DR0 sweeper. Disarm the old execute breakpoint
    // before collecting data writes so it cannot contaminate writer selection.
    g_dynamicHeadPhase.store(DynamicHeadPhase::Off, std::memory_order_release);
    SetDynamicHeadBreakpointAllThreads(oldWriter, false, true);
    for (auto& slot : g_dynamicHeadWriters)
    {
        slot.rip.store(0, std::memory_order_relaxed);
        slot.hits.store(0, std::memory_order_relaxed);
    }
    g_dynamicHeadWriterWatchHits.store(0, std::memory_order_release);
    g_dynamicHeadSource.store(source, std::memory_order_release);
    g_dynamicHeadPhase.store(DynamicHeadPhase::WatchWriter, std::memory_order_release);
    const auto watch=source+0x30;
    SetDynamicHeadBreakpointAllThreads(watch, true, false);
    std::uintptr_t selected=0;
    std::uint32_t selectedHits=0;
    const auto started=GetTickCount64();
    while (GetTickCount64()-started<1000 &&
        !g_dynamicHeadStopRequested.load(std::memory_order_acquire) && VrTrackingWanted())
    {
        Sleep(25);
        selected=0; selectedHits=0;
        for (auto& slot : g_dynamicHeadWriters)
        {
            const auto rip=slot.rip.load(std::memory_order_acquire);
            const auto hits=slot.hits.load(std::memory_order_acquire);
            if (rip>=g_exeBase && rip<g_exeBase+kRetailImageSize && hits>selectedHits)
            { selected=rip; selectedHits=hits; }
        }
        if (GetTickCount64()-started>=100 && selectedHits>=3) break;
    }
    g_dynamicHeadPhase.store(DynamicHeadPhase::Off, std::memory_order_release);
    SetDynamicHeadBreakpointAllThreads(watch, false, false);
    // Never install a selection for a camera that changed while it was watched.
    const bool accepted=selected && selectedHits>=3 &&
        g_cameraObserverLastSource.load(std::memory_order_acquire)==source &&
        IsDynamicHeadMatrix(source);
    writer=accepted ? selected : oldWriter;
    g_dynamicHeadSource.store(accepted ? source : oldSource, std::memory_order_release);
    g_dynamicHeadWriter.store(writer, std::memory_order_release);
    ResetHeadRecoveryHealth();
    if (g_dynamicHeadStopRequested.load(std::memory_order_acquire) || !VrTrackingWanted())
        return false;
    // Keep the existing pose center and stereo gate. Only move the writer hook.
    g_dynamicHeadPhase.store(DynamicHeadPhase::Active, std::memory_order_release);
    const auto armed=SetDynamicHeadBreakpointAllThreads(writer, true, true);
    Log("[HEADRECOVER] %s writer=+0x%llX samples=%u armed=%u; pose center and stereo preserved",
        accepted ? "reselected" : "no stable candidate; previous writer restored",
        static_cast<unsigned long long>(writer-g_exeBase), selectedHits, armed);
    return accepted;
}

// [MODETRACK] every non-failure exit of the worker.  A mode switch can land
// while this thread is unwinding, and the Start() that came with it saw
// "still running" and returned.  Re-check after releasing the flag, so head
// tracking always comes back by itself whenever a VR mode is on and no stop
// was requested.  StartDynamicHeadTracking's CAS keeps it to one worker.
void FinishDynamicHeadWorker()
{
    g_dynamicHeadPhase.store(DynamicHeadPhase::Off, std::memory_order_release);
    g_dynamicHeadWorkerRunning.store(false, std::memory_order_release);
    if (!g_dynamicHeadStopRequested.load(std::memory_order_acquire) && VrTrackingWanted())
    {
        Log("[MODETRACK] a VR mode is on as tracking shut down; restarting discovery");
        StartDynamicHeadTracking();
    }
}

DWORD WINAPI DynamicHeadWorker(LPVOID)
{
    const auto fail = [](const char* reason)
    {
        Log("[VRHEAD] dynamic discovery FAILED: %s; VR presentation remains active", reason);
        Beep(420, 220);
        g_dynamicHeadPhase.store(DynamicHeadPhase::Off, std::memory_order_release);
        g_dynamicHeadWorkerRunning.store(false, std::memory_order_release);
    };

    const auto captureSite = ResolveDynamicHeadCaptureSite();
    if (!captureSite)
    {
        fail("current-build getter-A pattern not found");
        return 0;
    }
    if (!g_dynamicHeadVeh)
        g_dynamicHeadVeh = AddVectoredExceptionHandler(1, &DynamicHeadHandler);
    if (!g_dynamicHeadVeh)
    {
        fail("vectored exception handler could not be installed");
        return 0;
    }

    ResetHeadRecoveryHealth();
    g_dynamicHeadRediscoverRequested.store(false, std::memory_order_release);
    g_dynamicHeadCaptureSite.store(captureSite, std::memory_order_release);
    g_dynamicHeadSource.store(0, std::memory_order_release);
    g_dynamicHeadCaptureHits.store(0, std::memory_order_release);
    g_dynamicHeadPhase.store(DynamicHeadPhase::CaptureSource, std::memory_order_release);
    const auto captureThreads = SetDynamicHeadBreakpointAllThreads(captureSite, true, true);
    Log("[VRHEAD] discovery stage 1/2: source capture armed at +0x%llX on %u threads for 3000 ms",
        static_cast<unsigned long long>(captureSite - g_exeBase), captureThreads);
    Sleep(3000);
    SetDynamicHeadBreakpointAllThreads(captureSite, false, true);
    if (g_dynamicHeadStopRequested.load(std::memory_order_acquire) ||
        !VrTrackingWanted())
    {
        FinishDynamicHeadWorker();
        return 0;
    }

    const auto source = g_dynamicHeadSource.load(std::memory_order_acquire);
    if (!source)
    {
        fail("gameplay camera source was not observed");
        return 0;
    }
    for (auto& slot : g_dynamicHeadWriters)
    {
        slot.rip.store(0, std::memory_order_relaxed);
        slot.hits.store(0, std::memory_order_relaxed);
    }
    const auto watch = source + 0x30;
    g_dynamicHeadWriterWatchHits.store(0, std::memory_order_release);
    g_dynamicHeadPhase.store(DynamicHeadPhase::WatchWriter, std::memory_order_release);
    const auto watchThreads = SetDynamicHeadBreakpointAllThreads(watch, true, false);
    Log("[VRHEAD] discovery stage 2/2: source=%p writer watch=%p on %u threads for 4000 ms",
        reinterpret_cast<void*>(source), reinterpret_cast<void*>(watch), watchThreads);
    Sleep(4000);
    SetDynamicHeadBreakpointAllThreads(watch, false, false);
    if (g_dynamicHeadStopRequested.load(std::memory_order_acquire) ||
        !VrTrackingWanted())
    {
        FinishDynamicHeadWorker();
        return 0;
    }

    std::uintptr_t writer = 0;
    std::uint32_t writerHits = 0;
    for (const auto& slot : g_dynamicHeadWriters)
    {
        const auto hits = slot.hits.load(std::memory_order_acquire);
        const auto rip = slot.rip.load(std::memory_order_acquire);
        if (rip && hits > writerHits)
        {
            writer = rip;
            writerHits = hits;
        }
    }
    if (!writer)
    {
        fail("no in-game writer touched the live camera source");
        return 0;
    }

    g_dynamicHeadWriter.store(writer, std::memory_order_release);
    RetailXr::ResetHeadPoseCenter();
    g_dynamicHeadPhase.store(DynamicHeadPhase::Active, std::memory_order_release);
    const auto activeThreads = SetDynamicHeadBreakpointAllThreads(writer, true, true);
    g_dynamicHeadLastSweepMs.store(GetTickCount(), std::memory_order_release);
    Log("[VRHEAD] ACTIVE source=%p writer=+0x%llX hits=%u armedThreads=%u",
        reinterpret_cast<void*>(source),
        static_cast<unsigned long long>(writer - g_exeBase), writerHits, activeThreads);
    Beep(2100, 100);
    Beep(2600, 120);
    const bool observeCamera = g_stereoEnabled.load(std::memory_order_acquire) &&
        !g_aerEnabled.load(std::memory_order_acquire);
    if (observeCamera)
    {
        g_cameraObserverSite.store(captureSite, std::memory_order_release);
        const auto observed = SetExecBreakpointAllThreads(2, captureSite, true, true);
        Log("[CAMOBS] passive getter observer armed=%u site=+0x%llX; tracking writer unchanged",
            observed, static_cast<unsigned long long>(captureSite - g_exeBase));
    }

    std::uint64_t lastSweep=GetTickCount64();
    std::uint64_t nextRecovery=0;
    while (!g_dynamicHeadStopRequested.load(std::memory_order_acquire) &&
        VrTrackingWanted())
    {
        Sleep(100);
        if (g_dynamicHeadStopRequested.load(std::memory_order_acquire) ||
            !VrTrackingWanted())
            break;
        const auto now=GetTickCount64();
        std::uintptr_t recoverySource=0;
        if (observeCamera && now>=nextRecovery &&
            TryAcquireSRWLockShared(&g_headRecoveryLock))
        {
            if (g_headRecoveryHealth.NeedsRecovery(now))
                recoverySource=g_headRecoveryHealth.candidate;
            ReleaseSRWLockShared(&g_headRecoveryLock);
        }
        if (recoverySource)
        {
            RecoverDynamicHeadWriter(recoverySource, writer);
            nextRecovery=GetTickCount64()+5000;
            lastSweep=GetTickCount64();
            continue;
        }
        if (g_dynamicHeadRediscoverRequested.exchange(false, std::memory_order_acq_rel))
        {
            Log("[VRHEAD] re-discovering the writer for a render-mode change");
            RecoverDynamicHeadWriter(source, writer);
            lastSweep=GetTickCount64();
            continue;
        }
        if (now-lastSweep<2000) continue;
        lastSweep=now;
        const auto swept = SetDynamicHeadBreakpointAllThreads(writer, true, true);
        if (observeCamera)
        {
            const auto observed = SetExecBreakpointAllThreads(2, captureSite, true, true);
            Log("[CAMOBS] tracked=%p getter=%p hits=%llu mismatches=%llu armed=%u",
                reinterpret_cast<void*>(g_dynamicHeadSource.load(std::memory_order_acquire)),
                reinterpret_cast<void*>(g_cameraObserverLastSource.load(std::memory_order_acquire)),
                static_cast<unsigned long long>(g_cameraObserverHits.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_cameraObserverMismatches.load(std::memory_order_relaxed)), observed);
        }
        g_dynamicHeadLastSweepMs.store(GetTickCount(), std::memory_order_release);
        if (!swept)
            Log("[VRHEAD] warning: active writer resweep reached zero threads");
    }
    if (observeCamera)
    {
        SetExecBreakpointAllThreads(2, captureSite, false, true);
        g_cameraObserverSite.store(0, std::memory_order_release);
    }
    SetDynamicHeadBreakpointAllThreads(writer, false, true);
    Log("[VRHEAD] dynamic camera ownership stopped");
    FinishDynamicHeadWorker();
    return 0;
}

void StartDynamicHeadTracking()
{
    // [MODETRACK] a VR mode is being turned on: cancel any stop still pending
    // from the mode that was just turned off.
    g_dynamicHeadStopRequested.store(false, std::memory_order_release);
    bool expected = false;
    if (!g_dynamicHeadWorkerRunning.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;
    g_dynamicHeadStopRequested.store(false, std::memory_order_release);
    if (const auto thread = CreateThread(nullptr, 0, &DynamicHeadWorker, nullptr, 0, nullptr))
    {
        CloseHandle(thread);
        Log("[VRHEAD] current-build dynamic camera discovery started");
    }
    else
    {
        g_dynamicHeadWorkerRunning.store(false, std::memory_order_release);
        Log("[VRHEAD] dynamic camera discovery thread could not start; stereo remains active");
    }
}

void StopDynamicHeadTracking()
{
    g_dynamicHeadStopRequested.store(true, std::memory_order_release);
}

bool DynamicHeadTrackingRunning()
{
    return g_dynamicHeadWorkerRunning.load(std::memory_order_acquire);
}

// [MODETRACK] the one thing both the VR-enable key and the Stereo/AER
// toggles now call instead of StartDynamicHeadTracking() directly: if no
// worker is running yet, start one exactly as before; if one is already
// ACTIVE (a writer was already found, just possibly the wrong mode's), ask
// it to re-scan without tearing anything down. Mid-discovery, do nothing -
// let the run already in flight finish.
void RediscoverDynamicHeadWriter()
{
    if (!DynamicHeadTrackingRunning())
    {
        StartDynamicHeadTracking();
        return;
    }
    if (g_dynamicHeadPhase.load(std::memory_order_acquire) == DynamicHeadPhase::Active)
        g_dynamicHeadRediscoverRequested.store(true, std::memory_order_release);
}

extern "C" void __fastcall RetailUpdateRenderViewPolicy(
    void* owner, float* view, float* inverse, std::uint32_t slot)
{
    g_headTrackCalls.fetch_add(1, std::memory_order_relaxed);
    bool applied = false;
    float headYaw = 0.0f;
    float headPitch = 0.0f;
    float headRoll = 0.0f;
    float headRight = 0.0f;
    float headUp = 0.0f;
    float headForward = 0.0f;
    std::uint64_t headSerial = 0;
    if (g_stereoEnabled.load(std::memory_order_acquire) && slot == 0 && view && inverse &&
        g_matrixInverse &&
        RetailXr::GetHeadPose(&headYaw, &headPitch, &headRoll,
            &headRight, &headUp, &headForward, &headSerial))
    {
        __try
        {
            alignas(16) float rotated[16]{};
            alignas(16) float rotatedInverse[16]{};
            std::memcpy(rotated, view, sizeof(rotated));
            // Signs as established by the headlook probe.
            const bool sixDof =
                g_sixDofEnabled.load(std::memory_order_acquire);
            const float positionScale = sixDof
                ? g_sixDofPositionScale.load(std::memory_order_acquire) : 0.0f;
            if (sixDof)
                ApplyCameraLocalOffset(rotated, headRight * positionScale,
                    headUp * positionScale, -headForward * positionScale);
            const float uicamBase2[3]{rotated[2], rotated[6], rotated[10]};
            if (ApplyHeadRotation(rotated, -headYaw, headPitch, 0.0f))
            {
                {
                    const float uicamHead2[3]{rotated[2], rotated[6], rotated[10]};
                    UiHook::SetCameraAxes(uicamBase2, uicamHead2);
                }
                g_matrixInverse(rotated, rotatedInverse);
                std::memcpy(view, rotated, sizeof(rotated));
                std::memcpy(inverse, rotatedInverse, sizeof(rotatedInverse));
                applied = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            applied = false;
        }
    }

    if (applied)
    {
        RetailXr::MarkHeadPoseRendered(headSerial);
        const auto count = g_headTrackApplied.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8 || (count % 600) == 0)
            Log("[VRHEAD] applied=%llu serial=%llu yaw=%.4f pitch=%.4f slot=%u view=%p",
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(headSerial), headYaw, headPitch, slot, view);
    }
    else if (g_stereoEnabled.load(std::memory_order_relaxed) && slot == 0)
    {
        g_headTrackRejected.fetch_add(1, std::memory_order_relaxed);
    }

    if (g_realUpdateRenderView)
        g_realUpdateRenderView(owner, view, inverse, slot);
}

bool TryInstallHeadTrackingHook()
{
    if (g_updateRenderViewPatch.installed)
        return true;
    const auto site = g_exeBase + kUpdateRenderViewARva;
    if (!Matches(site, kUpdateRenderViewAPrefix))
    {
        const auto attempt = g_headTrackHookAttempts.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (attempt <= 3 || (attempt % 20) == 0)
        {
            std::uint8_t bytes[15]{};
            bool readable = false;
            __try
            {
                std::memcpy(bytes, reinterpret_cast<const void*>(site), sizeof(bytes));
                readable = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            Log("[VRHEAD] hook wait attempt=%llu readable=%d bytes="
                "%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X "
                "%02X%02X%02X%02X%02X",
                static_cast<unsigned long long>(attempt), readable ? 1 : 0,
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
                bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14]);
        }
        return false;
    }
    g_matrixInverse = reinterpret_cast<MatrixInverseFn>(g_exeBase + kMatrixInverseRva);
    if (!WriteTrampolineJump(g_updateRenderViewPatch, site,
            kUpdateRenderViewAPrefix.size(),
            reinterpret_cast<std::uintptr_t>(&RetailUpdateRenderViewPolicy),
            reinterpret_cast<void**>(&g_realUpdateRenderView)))
        return false;
    Log("[VRHEAD] optional UPDATEVIEW_A hook installed at +0x%llX",
        static_cast<unsigned long long>(kUpdateRenderViewARva));
    return true;
}

bool ValidateRetailExecutable()
{
    g_exeBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_exeBase)
        return false;
    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exeBase);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_exeBase + dos->e_lfanew);
        return dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE &&
            nt->FileHeader.TimeDateStamp == kRetailTimestamp &&
            nt->OptionalHeader.SizeOfImage == kRetailImageSize;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// [AERSYNC] One eye "changes" while driving in AER.
//
// AER renders the two eyes of a pair on two consecutive game frames.  EYEHOLD
// gives the second eye the first eye's game camera, so the static world lines
// up, but the simulation still advances on that frame: the car, the party and
// every NPC are one frame further on than the camera that looks at them.  At
// driving speed that is tens of centimetres, in one eye only.  No camera-side
// change can repair it: releasing the hold moves the error to the world.
//
// The engine's frame update FUN_1404d04b0 hands the frame's elapsed time to
// FUN_142dbec90(world, &dt, &unpausedDt, passes), which stores it in the task
// world (+0x204 / +0x208) and runs the tasks.  The engine passes dt = 0 itself
// when the game is paused, and the task invoker (+0x2DC1D60) skips a callback
// whose dt is 0, so a zero frame is a native state.
//
// Here the frame whose camera build will be the SECOND eye gets dt = 0, and
// its elapsed time is carried into the next frame, which is the first eye of
// the next pair.  Both eyes of a pair then show one simulation moment, and
// game speed is unchanged.  The unpaused clock (menus, UI) is left alone.
// Units are 1/300000 s.  Never two zero frames in a row, so a camera build
// that does not happen cannot freeze the game.
constexpr std::uintptr_t kWorldStepRva = 0x02DBEC90;           // FUN_142dbec90
constexpr std::array<std::uint8_t, 24> kWorldStepPrefix{
    0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x40, 0xA8, 0xFE, 0xFF, 0xFF, 0xFF};
using WorldStepFn = void(__fastcall*)(std::uint32_t, std::int32_t*, std::uint32_t*, std::uint32_t);
WorldStepFn g_realWorldStep{};
std::atomic_bool g_aerSimSync{true};
std::atomic_bool g_aerSyncInstalled{};
std::int32_t g_aerSyncCarry = 0;            // game thread only
bool g_aerSyncLastZero = false;             // game thread only
std::uint64_t g_aerSyncZero = 0, g_aerSyncFed = 0, g_aerSyncFrames = 0, g_aerSyncForced = 0;
std::int32_t g_aerSyncLastDt = 0, g_aerSyncLastFed = 0;
int g_aerSyncLastState = -1;
// Self-check.  With the right frame held, the game's own camera at the second
// eye's build is where it was at the first eye's build; with the wrong one it
// has moved a whole pair's worth.  The camera-build handler measures both
// (micrometres, summed); the update reads them and corrects which frame it
// holds.  The engine's order of update and camera build is not assumed.
int g_aerSyncPhase = 0;                     // game thread only
std::uint64_t g_aerSyncFlips = 0;

void __fastcall WorldStepDetour(std::uint32_t world, std::int32_t* dt, std::uint32_t* unpausedDt,
    std::uint32_t passes)
{
    // The update calls this twice per frame (world 0, then world 1) with the
    // same two pointers; the decision is taken once, on world 0.
    if (world == 0 && dt)
    {
        const bool active = g_aerSimSync.load(std::memory_order_relaxed) &&
            g_aerEnabled.load(std::memory_order_acquire) &&
            g_eyeHoldEnabled.load(std::memory_order_acquire) &&
            !g_afwEnabled.load(std::memory_order_acquire) &&
            !RetailXr::GetMonoSubmit() && !RetailXr::GetSceneIsFlat() &&
            g_dynamicHeadPhase.load(std::memory_order_acquire) == DynamicHeadPhase::Active;
        const int state = active ? 1 : 0;
        if (state != g_aerSyncLastState)
        {
            g_aerSyncLastState = state;
            Log("[AERSYNC] %s", active
                ? "ON: the second eye of each pair renders the same game moment as the first"
                : "off (not AER, flat scene, Mono, or head tracking not running)");
        }
        if (!active)
        {
            // Hand back whatever was carried, once, so no time is lost.
            if (g_aerSyncCarry > 0 && *dt > 0)
                *dt += g_aerSyncCarry;
            g_aerSyncCarry = 0;
            g_aerSyncLastZero = false;
        }
        else
        {
            ++g_aerSyncFrames;
            const std::int32_t real = *dt;
            if (g_aerPairSamples.load(std::memory_order_relaxed) >= 90)
            {
                const auto gap = g_aerPairGapUm.exchange(0, std::memory_order_relaxed);
                const auto move = g_aerPairMoveUm.exchange(0, std::memory_order_relaxed);
                const auto samples = g_aerPairSamples.exchange(0, std::memory_order_relaxed);
                // Camera moved at least 2 cm per pair on average, and the second
                // eye saw more than a third of it: the wrong frame is held.
                const bool moving = move > samples * 20000ull;
                // Measured while driving: phase 0 (hold the frame
                // whose build is eye 1) leaves the second eye 0.001-0.014 m off
                // against 0.15-0.60 m of travel per pair; phase 1 leaves it a
                // full pair off.  On foot the camera also moves on the unpaused
                // clock, which reads as "half off" in EITHER phase, so letting
                // this check flip the phase made it thrash.  Phase 0 is fixed;
                // the check only reports.
                const bool wrong = moving && gap * 3 > move;
                static std::uint64_t checks = 0;
                if (wrong || ++checks <= 8 || (checks % 40) == 0)
                    Log("[AERSYNC] pair check over %llu pairs: camera moved %.3f m per pair, second eye "
                        "was %.3f m off the first -> %s (phase %d, corrections %llu)",
                        static_cast<unsigned long long>(samples),
                        samples ? double(move) / double(samples) * 1e-6 : 0.0,
                        samples ? double(gap) / double(samples) * 1e-6 : 0.0,
                        !moving ? "camera still, nothing to judge"
                                : (wrong ? "second eye still moved (camera on the unpaused clock)"
                                         : "same game moment"),
                        g_aerSyncPhase, static_cast<unsigned long long>(g_aerSyncFlips));
            }
            const bool secondEye = (RetailXr::PeekAerBuildEye() ^ g_aerSyncPhase) == 1;
            if (secondEye && !g_aerSyncLastZero && real > 0)
            {
                g_aerSyncCarry += real;
                // The engine caps a frame at 0.1 s (30000); keep the carry
                // inside what it would accept.
                if (g_aerSyncCarry > 30000)
                    g_aerSyncCarry = 30000;
                *dt = 0;
                g_aerSyncLastZero = true;
                g_aerSyncLastDt = real;
                ++g_aerSyncZero;
            }
            else
            {
                if (secondEye && g_aerSyncLastZero)
                    ++g_aerSyncForced;
                if (real > 0)
                {
                    *dt = real + g_aerSyncCarry;
                    g_aerSyncLastFed = *dt;
                    ++g_aerSyncFed;
                }
                g_aerSyncCarry = 0;
                g_aerSyncLastZero = false;
            }
            if (g_aerSyncFrames <= 6 || (g_aerSyncFrames % 1200) == 0)
                Log("[AERSYNC] frames=%llu held=%llu advanced=%llu forcedAdvance=%llu | last held frame %d, "
                    "last advanced frame %d (1/300000 s) | next eye=%d",
                    static_cast<unsigned long long>(g_aerSyncFrames),
                    static_cast<unsigned long long>(g_aerSyncZero),
                    static_cast<unsigned long long>(g_aerSyncFed),
                    static_cast<unsigned long long>(g_aerSyncForced),
                    g_aerSyncLastDt, g_aerSyncLastFed, RetailXr::PeekAerBuildEye());
        }
    }
    g_realWorldStep(world, dt, unpausedDt, passes);
}

bool InstallAerSimSync()
{
    if (!Matches(g_exeBase + kWorldStepRva, kWorldStepPrefix))
    {
        Log("[AERSYNC] NOT installed: world-step bytes differ from the analysed exe");
        return false;
    }
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("[AERSYNC] NOT installed: MinHook init %s", MH_StatusToString(init));
        return false;
    }
    auto* target = reinterpret_cast<void*>(g_exeBase + kWorldStepRva);
    auto result = MH_CreateHook(target, reinterpret_cast<void*>(&WorldStepDetour),
        reinterpret_cast<void**>(&g_realWorldStep));
    if (result == MH_OK)
        result = MH_EnableHook(target);
    g_aerSyncInstalled.store(result == MH_OK, std::memory_order_release);
    Log("[AERSYNC] world step +0x%llX %s", static_cast<unsigned long long>(kWorldStepRva),
        result == MH_OK ? "hooked: AER eye pairs share one game moment"
                        : MH_StatusToString(result));
    return result == MH_OK;
}

// [VOLGUARD] Stereo crashed in the Leviathan set piece
// inside the game's volumetric light pass (VolumetricLightManager):
// ffxv_s.exe+0x2C7F80A reads [texture+8] where the texture is null.  Chain:
// view task +0xE8D68C0 -> +0x2C7B290(manager, ctx, view) -> +0x2C7EB50.  The
// pass picks its source target as [[view+0x1E58]+8]+0x8F0 when
// (view+0x1DFF & 6) == 6 and [[view+0x708]+0x18] == 0 (FUN +0x2C86640);
// otherwise [[view+0x1E48]+0x10]+0x750, which is never null.  That target was
// missing on the view being drawn.  Only that pass is skipped, only on a view
// whose target is missing: the fog and light shafts drop out for that frame
// instead of the game crashing.  Every other frame is untouched.
constexpr std::uintptr_t kVolumetricEntryRva = 0x02C7B290;    // FUN_142c7b290
constexpr std::array<std::uint8_t, 16> kVolumetricEntryPrefix{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
    0x48, 0x89, 0x7C, 0x24, 0x18, 0x55};
using VolumetricEntryFn = void(__fastcall*)(void*, void*, std::uint8_t*);
VolumetricEntryFn g_realVolumetricEntry{};
std::atomic_uint64_t g_volGuardSkips{}, g_volGuardCalls{};

bool VolumetricTargetMissing(const std::uint8_t* view)
{
    __try
    {
        if ((view[0x1DFF] & 6) != 6)
            return false;
        const auto state = *reinterpret_cast<const std::uintptr_t*>(view + 0x708);
        if (!state || *reinterpret_cast<const std::uint8_t*>(state + 0x18) != 0)
            return false;
        const auto set = *reinterpret_cast<const std::uintptr_t*>(view + 0x1E58);
        if (!set)
            return true;
        const auto inner = *reinterpret_cast<const std::uintptr_t*>(set + 8);
        if (!inner)
            return true;
        return *reinterpret_cast<const std::uintptr_t*>(inner + 0x8F0) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return true;
    }
}

void __fastcall VolumetricEntryDetour(void* manager, void* context, std::uint8_t* view)
{
    g_volGuardCalls.fetch_add(1, std::memory_order_relaxed);
    if (view && VolumetricTargetMissing(view))
    {
        const auto n = g_volGuardSkips.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10 || (n % 600) == 0)
            Log("[VOLGUARD] volumetric light skipped: its source target is missing on this view "
                "(skips=%llu of %llu calls, stereo=%d aer=%d)",
                static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(g_volGuardCalls.load(std::memory_order_relaxed)),
                g_stereoEnabled.load(std::memory_order_relaxed) ? 1 : 0,
                g_aerEnabled.load(std::memory_order_relaxed) ? 1 : 0);
        return;
    }
    g_realVolumetricEntry(manager, context, view);
}

bool InstallVolumetricGuard()
{
    if (!Matches(g_exeBase + kVolumetricEntryRva, kVolumetricEntryPrefix))
    {
        Log("[VOLGUARD] NOT installed: volumetric-light bytes differ from the analysed exe");
        return false;
    }
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    auto* target = reinterpret_cast<void*>(g_exeBase + kVolumetricEntryRva);
    auto result = MH_CreateHook(target, reinterpret_cast<void*>(&VolumetricEntryDetour),
        reinterpret_cast<void**>(&g_realVolumetricEntry));
    if (result == MH_OK)
        result = MH_EnableHook(target);
    Log("[VOLGUARD] volumetric light +0x%llX %s", static_cast<unsigned long long>(kVolumetricEntryRva),
        result == MH_OK ? "guarded: a missing source target skips the pass instead of crashing"
                        : MH_StatusToString(result));
    return result == MH_OK;
}

bool InstallEnginePatches()
{
    const auto displayGetter = g_exeBase + kDisplayGetterRva;
    const auto outputGetter = g_exeBase + kCurrentOutputGetterRva;
    const auto stripped = g_exeBase + kStrippedMultiOutputRva;
    const auto full = g_exeBase + kFullMultiOutputRva;
    const auto matrixCache = g_exeBase + kMatrixCacheWriterRva;
    const auto matrixCacheConstructor = g_exeBase + kMatrixCacheConstructorRva;

    const auto poseRecorder = g_exeBase + kPoseBufferRecorderRva;
    const auto passQueue = g_exeBase + kPassQueueRva;
    const auto uploadMatrices = g_exeBase + kUploadMatricesRva;
    const auto replayCopyIat = g_exeBase + kReplayCopyIatRva;
    const auto actorLodBudget = g_exeBase + kActorLodBudgetRva;
    const auto actorLodManagerRun = g_exeBase + kActorLodManagerRunRva;
    const auto actorPointLodKernel = g_exeBase + kActorPointLodKernelRva;
    const auto actorBoundedLodKernel = g_exeBase + kActorBoundedLodKernelRva;
    const auto replayDispatchCall = g_exeBase + kReplayDispatchCallRva;
    const auto replayMapCall = g_exeBase + kReplayMapCallRva;
    const auto replayDrawCallA = g_exeBase + kReplayDrawCallARva;
    const auto replayDrawCallB = g_exeBase + kReplayDrawCallBRva;
    const auto replayDrawContinue = g_exeBase + kReplayDrawContinueRva;
    const auto eyeViewBuilderCall = g_exeBase + kEyeViewBuilderCallRva;
    const auto eyeProjectionBuilderCall = g_exeBase + kEyeProjectionBuilderCallRva;
    const auto eyeViewBuilderTarget = g_exeBase + kEyeViewBuilderTargetRva;
    const auto eyeProjectionBuilderTarget = g_exeBase + kEyeProjectionBuilderTargetRva;
    const auto sepProbeProjectionCall = g_exeBase + kSepProbeProjectionCallRva;
    const auto sepProbeViewCall = g_exeBase + kSepProbeViewCallRva;
    const auto sepProbeProjectionTarget = g_exeBase + kSepProbeProjectionTargetRva;
    const auto sepProbeViewTarget = g_exeBase + kSepProbeViewTargetRva;

    if (!Matches(displayGetter, kDisplayGetterPrefix) ||
        !Matches(outputGetter, kCurrentOutputPrefix) ||
        !Matches(stripped, kStrippedMultiOutputPrefix) ||
        !Matches(full, kFullMultiOutputPrefix) ||
        !Matches(matrixCache, kMatrixCacheWriterPrefix) ||
        !Matches(matrixCacheConstructor, kMatrixCacheConstructorPrefix) ||

        !Matches(poseRecorder, kPoseBufferRecorderPrefix) ||
        !Matches(passQueue, kPassQueuePrefix) ||
        !Matches(uploadMatrices, kUploadMatricesPrefix) ||
        !Matches(actorLodBudget, kActorLodBudgetPrefix) ||
        !Matches(actorLodManagerRun, kActorLodManagerRunPrefix) ||
        !Matches(actorPointLodKernel, kActorPointLodKernelPrefix) ||
        !Matches(actorBoundedLodKernel, kActorBoundedLodKernelPrefix) ||
        !Matches(replayDispatchCall, kReplayDispatchCallPrefix) ||
        !Matches(replayMapCall, kReplayMapCallPrefix) ||
        !Matches(replayDrawCallA, kReplayDrawCallAPrefix) ||
        !Matches(replayDrawCallB, kReplayDrawCallBPrefix))
        return false;

    if (!Matches(g_exeBase + kPostChainRvas[0], kPostChain0Prefix) ||
        !Matches(g_exeBase + kPostChainRvas[1], kPostChain1Prefix) ||
        !Matches(g_exeBase + kPostChainRvas[2], kPostChain2Prefix) ||
        !Matches(g_exeBase + kPostChainRvas[3], kPostChain3Prefix) ||
        !Matches(g_exeBase + kPostChainRvas[4], kPostChain4Prefix) ||
        !Matches(g_exeBase + kPostChainRvas[5], kPostChain5Prefix) ||
        !Matches(g_exeBase + kBankRotatorRva, kBankRotatorPrefix) ||
        !Matches(g_exeBase + kPassRegistrarRva, kPassRegistrarPrefix))
        return false;

    g_matrixCacheConstructor = reinterpret_cast<MatrixCacheConstructor>(matrixCacheConstructor);
    g_matrixInverse = reinterpret_cast<MatrixInverseFn>(g_exeBase + kMatrixInverseRva);

    const auto restoreAll = []()
    {
        RestoreCodePatch(g_updateDispatchPatch);
        RestoreCodePatch(g_qualityMaskPatch);
        RestoreCodePatch(g_probeWeightsPatch);
        RestoreCodePatch(g_fiberOutputPatch);
        if (g_fiberOutputRelay)
        {
            VirtualFree(g_fiberOutputRelay, 0, MEM_RELEASE);
            g_fiberOutputRelay = nullptr;
        }
        RestoreCodePatch(g_passRegistrarPatch);
        RestoreCodePatch(g_bankRotatorPatch);
        for (auto& patch : g_postChainPatches)
            RestoreCodePatch(patch);
        RestoreCodePatch(g_multiOutputPatch);
        RestoreCodePatch(g_outputGetterPatch);
        RestoreCodePatch(g_matrixCachePatch);
        RestoreCodePatch(g_updateRenderViewPatch);
        RestoreCodePatch(g_poseBufferRecorderPatch);
        RestoreCodePatch(g_uploadMatricesPatch);
        RestoreCodePatch(g_eyeViewBuilderCallPatch);
        RestoreCodePatch(g_eyeProjectionBuilderCallPatch);
        RestoreCodePatch(g_sepProbeProjectionCallPatch);
        RestoreCodePatch(g_sepProbeViewCallPatch);
        if (g_eyeViewBuilderRelay)
        {
            VirtualFree(g_eyeViewBuilderRelay, 0, MEM_RELEASE);
            g_eyeViewBuilderRelay = nullptr;
        }
        if (g_eyeProjectionBuilderRelay)
        {
            VirtualFree(g_eyeProjectionBuilderRelay, 0, MEM_RELEASE);
            g_eyeProjectionBuilderRelay = nullptr;
        }
        if (g_sepProbeProjectionRelay)
        {
            VirtualFree(g_sepProbeProjectionRelay, 0, MEM_RELEASE);
            g_sepProbeProjectionRelay = nullptr;
        }
        if (g_sepProbeViewRelay)
        {
            VirtualFree(g_sepProbeViewRelay, 0, MEM_RELEASE);
            g_sepProbeViewRelay = nullptr;
        }
        RestoreCodePatch(g_passQueuePatch);
        RestoreCodePatch(g_replayCopyIatPatch);
        RestoreCodePatch(g_actorLodBudgetPatch);
        RestoreCodePatch(g_actorLodManagerRunPatch);
        RestoreCodePatch(g_actorPointLodKernelPatch);
        RestoreCodePatch(g_actorBoundedLodKernelPatch);
        RestoreCodePatch(g_replayDispatchCallPatch);
        RestoreCodePatch(g_replayMapCallPatch);
        RestoreCodePatch(g_replayDrawCallAPatch);
        RestoreCodePatch(g_replayDrawCallBPatch);
        if (g_replayDispatchRelay)
        {
            VirtualFree(g_replayDispatchRelay, 0, MEM_RELEASE);
            g_replayDispatchRelay = nullptr;
        }
        if (g_replayMapRelay)
        {
            VirtualFree(g_replayMapRelay, 0, MEM_RELEASE);
            g_replayMapRelay = nullptr;
        }
        if (g_replayDrawRelayA)
        {
            VirtualFree(g_replayDrawRelayA, 0, MEM_RELEASE);
            g_replayDrawRelayA = nullptr;
        }
        if (g_replayDrawRelayB)
        {
            VirtualFree(g_replayDrawRelayB, 0, MEM_RELEASE);
            g_replayDrawRelayB = nullptr;
        }
    };


    if (!WriteReplayMapViaRelay(g_replayMapCallPatch, replayMapCall,
            reinterpret_cast<std::uintptr_t>(&RetailReplayMapPolicy), &g_replayMapRelay) ||
        !WriteReplayDrawTailViaRelay(g_replayDrawCallAPatch, replayDrawCallA,
            kReplayDrawCallAPrefix, reinterpret_cast<std::uintptr_t>(&RetailReplayDrawPolicy),
            replayDrawContinue, &g_replayDrawRelayA) ||
        !WriteReplayDrawTailViaRelay(g_replayDrawCallBPatch, replayDrawCallB,
            kReplayDrawCallBPrefix, reinterpret_cast<std::uintptr_t>(&RetailReplayDrawPolicy),
            replayDrawContinue, &g_replayDrawRelayB) ||
        !WriteRelativeCallViaRelay(g_replayDispatchCallPatch, replayDispatchCall,
            reinterpret_cast<std::uintptr_t>(&RetailReplayDispatchPolicy),
            &g_replayDispatchRelay) ||
        !WriteTrampolineJump(g_actorBoundedLodKernelPatch, actorBoundedLodKernel, 15,
            reinterpret_cast<std::uintptr_t>(&RetailActorBoundedLodKernelPolicy),
            reinterpret_cast<void**>(&g_realActorBoundedLodKernel)) ||
        !WriteTrampolineJump(g_actorPointLodKernelPatch, actorPointLodKernel, 20,
            reinterpret_cast<std::uintptr_t>(&RetailActorPointLodKernelPolicy),
            reinterpret_cast<void**>(&g_realActorPointLodKernel)) ||
        !WriteTrampolineJump(g_actorLodManagerRunPatch, actorLodManagerRun, 20,
            reinterpret_cast<std::uintptr_t>(&RetailActorLodManagerRunPolicy),
            reinterpret_cast<void**>(&g_realActorLodManagerRun)) ||
        !WriteTrampolineJump(g_actorLodBudgetPatch, actorLodBudget, 17,
            reinterpret_cast<std::uintptr_t>(&RetailActorLodBudgetPolicy),
            reinterpret_cast<void**>(&g_realActorLodBudget)) ||
        !WritePointerPatch(g_replayCopyIatPatch, replayCopyIat,
            reinterpret_cast<const void*>(&HookReplayCopy),
            reinterpret_cast<void**>(&g_realReplayCopy)) ||
        !WriteTrampolineJump(g_passQueuePatch, passQueue, 20,
            reinterpret_cast<std::uintptr_t>(&RetailPassQueuePolicy),
            reinterpret_cast<void**>(&g_realPassQueue)) ||
        !WriteTrampolineJump(g_uploadMatricesPatch, uploadMatrices, 16,
            reinterpret_cast<std::uintptr_t>(&RetailUploadMatricesPolicy),
            reinterpret_cast<void**>(&g_realUploadMatrices)) ||
        !WriteTrampolineJump(g_poseBufferRecorderPatch, poseRecorder, 16,
            reinterpret_cast<std::uintptr_t>(&RetailPoseBufferRecorderPolicy),
            reinterpret_cast<void**>(&g_realPoseBufferRecorder)))
    {
        restoreAll();
        return false;
    }
    if (!WriteTrampolineJump(g_postChainPatches[0], g_exeBase + kPostChainRvas[0],
            kPostChainStolen[0], reinterpret_cast<std::uintptr_t>(&RetailPostChain0Policy),
            reinterpret_cast<void**>(&g_realPostChain0)) ||
        !WriteTrampolineJump(g_postChainPatches[1], g_exeBase + kPostChainRvas[1],
            kPostChainStolen[1], reinterpret_cast<std::uintptr_t>(&RetailPostChain1Policy),
            reinterpret_cast<void**>(&g_realPostChain1)) ||
        !WriteTrampolineJump(g_postChainPatches[2], g_exeBase + kPostChainRvas[2],
            kPostChainStolen[2], reinterpret_cast<std::uintptr_t>(&RetailPostChain2Policy),
            reinterpret_cast<void**>(&g_realPostChain2)) ||
        !WriteTrampolineJump(g_postChainPatches[3], g_exeBase + kPostChainRvas[3],
            kPostChainStolen[3], reinterpret_cast<std::uintptr_t>(&RetailPostChain3Policy),
            reinterpret_cast<void**>(&g_realPostChain3)) ||
        !WriteTrampolineJump(g_postChainPatches[4], g_exeBase + kPostChainRvas[4],
            kPostChainStolen[4], reinterpret_cast<std::uintptr_t>(&RetailPostChain4Policy),
            reinterpret_cast<void**>(&g_realPostChain4)) ||
        !WriteTrampolineJump(g_postChainPatches[5], g_exeBase + kPostChainRvas[5],
            kPostChainStolen[5], reinterpret_cast<std::uintptr_t>(&RetailPostChain5Policy),
            reinterpret_cast<void**>(&g_realPostChain5)) ||
        !WriteTrampolineJump(g_bankRotatorPatch, g_exeBase + kBankRotatorRva, 14,
            reinterpret_cast<std::uintptr_t>(&RetailBankRotatorPolicy),
            reinterpret_cast<void**>(&g_realBankRotator)) ||
        !WriteTrampolineJump(g_passRegistrarPatch, g_exeBase + kPassRegistrarRva, 15,
            reinterpret_cast<std::uintptr_t>(&RetailPassRegistrarPolicy),
            reinterpret_cast<void**>(&g_realPassRegistrar)))
    {
        restoreAll();
        return false;
    }
    g_realSetCurrentOutput = reinterpret_cast<SetCurrentOutputFn>(
        g_exeBase + kSetCurrentOutputRva);
    if (!WriteRelativeJumpViaRelay(g_fiberOutputPatch,
            g_exeBase + kFiberOutputRestoreJmpRva,
            reinterpret_cast<std::uintptr_t>(&RetailFiberOutputRestorePolicy),
            &g_fiberOutputRelay))
    {
        restoreAll();
        return false;
    }
    if (!WriteTrampolineJump(g_probeWeightsPatch, g_exeBase + kProbeWeightsRva,
            kProbeWeightsStolen,
            reinterpret_cast<std::uintptr_t>(&RetailProbeWeightsPolicy),
            reinterpret_cast<void**>(&g_realProbeWeights)))
    {
        restoreAll();
        return false;
    }
    if (!WriteTrampolineJump(g_updateDispatchPatch, g_exeBase + kUpdateDispatchRva,
            kUpdateDispatchStolen,
            reinterpret_cast<std::uintptr_t>(&RetailUpdateDispatchPolicy),
            reinterpret_cast<void**>(&g_realUpdateDispatch)))
    {
        restoreAll();
        return false;
    }
    if (!WriteTrampolineJump(g_qualityMaskPatch, g_exeBase + kQualityMaskApplyRva,
            kQualityMaskStolen,
            reinterpret_cast<std::uintptr_t>(&RetailQualityMaskApplyPolicy),
            reinterpret_cast<void**>(&g_realQualityMask)))
    {
        restoreAll();
        return false;
    }
    if (!WriteTrampolineJump(g_matrixCachePatch, matrixCache, 15,
            reinterpret_cast<std::uintptr_t>(&RetailMatrixCachePolicy),
            reinterpret_cast<void**>(&g_realMatrixCacheWriter)))
    {
        restoreAll();
        return false;
    }
    if (!WriteAbsoluteJump(g_outputGetterPatch, outputGetter,
            reinterpret_cast<std::uintptr_t>(&RetailCurrentOutputPolicy)))
    {
        restoreAll();
        return false;
    }
    g_realEyeViewBuilder = reinterpret_cast<EyeMatrixBuilderFn>(eyeViewBuilderTarget);
    g_realEyeProjectionBuilder = reinterpret_cast<EyeMatrixBuilderFn>(
        eyeProjectionBuilderTarget);
    g_realSepProbeProjectionUpload = reinterpret_cast<SepProbeUploadFn>(
        sepProbeProjectionTarget);
    g_realSepProbeViewUpload = reinterpret_cast<SepProbeUploadFn>(sepProbeViewTarget);
    g_matrixInverse = reinterpret_cast<MatrixInverseFn>(g_exeBase + kMatrixInverseRva);
    if (!WriteValidatedProbeCallViaRelay(g_eyeViewBuilderCallPatch,
            eyeViewBuilderCall, eyeViewBuilderTarget,
            reinterpret_cast<std::uintptr_t>(&RetailEyeViewBuilder),
            &g_eyeViewBuilderRelay) ||
        !WriteValidatedProbeCallViaRelay(g_eyeProjectionBuilderCallPatch,
            eyeProjectionBuilderCall, eyeProjectionBuilderTarget,
            reinterpret_cast<std::uintptr_t>(&RetailEyeProjectionBuilder),
            &g_eyeProjectionBuilderRelay) ||
        !WriteValidatedProbeCallViaRelay(g_sepProbeProjectionCallPatch,
            sepProbeProjectionCall, sepProbeProjectionTarget,
            reinterpret_cast<std::uintptr_t>(&RetailSepProbeProjectionUpload),
            &g_sepProbeProjectionRelay) ||
        !WriteValidatedProbeCallViaRelay(g_sepProbeViewCallPatch,
            sepProbeViewCall, sepProbeViewTarget,
            reinterpret_cast<std::uintptr_t>(&RetailSepProbeViewUpload),
            &g_sepProbeViewRelay))
    {
        restoreAll();
        return false;
    }
    Log("[EYEPAIR] native builders hooked at view +0x%llX and projection +0x%llX; "
        "render commits hooked at +0x%llX/+0x%llX",
        static_cast<unsigned long long>(kEyeViewBuilderCallRva),
        static_cast<unsigned long long>(kEyeProjectionBuilderCallRva),
        static_cast<unsigned long long>(kSepProbeProjectionCallRva),
        static_cast<unsigned long long>(kSepProbeViewCallRva));
    g_realCameraJob = reinterpret_cast<MultiOutputCameraJobFn>(full);
    if constexpr (!kNativeStereoOnly)
    {
        if (!WriteAbsoluteJump(g_multiOutputPatch, stripped,
                reinterpret_cast<std::uintptr_t>(&RetailCameraJobPolicy)))
        {
            restoreAll();
            return false;
        }
    }
    else
    {
        Log("[NATIVEONLY] camera-job substitution SKIPPED - retail stripped job left in place");
    }
    if (g_camFixEnabled.load(std::memory_order_acquire))
        ApplyCamFix(true);
    if constexpr (!kNativeStereoOnly)
        StereoTaskFix::Install(g_exeBase, +[]() -> bool {
            return g_stereoEnabled.load(std::memory_order_acquire) &&
                !g_stereoSuspended.load(std::memory_order_acquire);
        }, &Log);
    g_enginePatchesReady.store(true, std::memory_order_release);
    // [VRESTORE] lighting-state restore replaced (verified first).
    if (Matches(g_exeBase + kViewRestoreRva, kViewRestorePrefix) &&
        WriteAbsoluteJump(g_viewRestorePatch, g_exeBase + kViewRestoreRva,
            reinterpret_cast<std::uintptr_t>(&ViewRestoreReplacement)))
        Log("[VRESTORE] view-quality restore +0x%llX replaced: stereo keeps full probe lighting inside the restore itself",
            static_cast<unsigned long long>(kViewRestoreRva));
    else
        Log("[VRESTORE] NOT installed: restore bytes differ from the analysed exe");
    InstallOutputBeginHook();   // [HISTFIX]
    InstallAerSimSync();        // [AERSYNC]
    InstallVolumetricGuard();   // [VOLGUARD]
    // [LENSPIN] both lens functions, verified byte for byte first.
    if (Matches(g_exeBase + kLensGetterRva, kLensGetterPrefix) &&
        Matches(g_exeBase + kLensSiblingRva, kLensSiblingPrefix) &&
        WriteAbsoluteJump(g_lensGetterPatch, g_exeBase + kLensGetterRva,
            reinterpret_cast<std::uintptr_t>(&LensGetterDetour)) &&
        WriteAbsoluteJump(g_lensSiblingPatch, g_exeBase + kLensSiblingRva,
            reinterpret_cast<std::uintptr_t>(&LensSiblingDetour)))
        Log("[LENSPIN] lens getter +0x%llX and sibling +0x%llX now return the pinned lens for every camera while VR is on",
            static_cast<unsigned long long>(kLensGetterRva),
            static_cast<unsigned long long>(kLensSiblingRva));
    else
        Log("[LENSPIN] NOT installed: lens function bytes differ from the analysed exe (getter %d sibling %d)",
            Matches(g_exeBase + kLensGetterRva, kLensGetterPrefix) ? 1 : 0,
            Matches(g_exeBase + kLensSiblingRva, kLensSiblingPrefix) ? 1 : 0);
    Log("engine patches ready: +0x%llX -> full job +0x%llX; POSTSKIP eye-1 duplicate skip (6 sites, F8 toggles), output policy, matrix-cache guard, replay-source census, PASSQ, passive UPMTX, ACTORLOD, LODVIEW, LODPOINT, and LODBOUND census installed",
        static_cast<unsigned long long>(kStrippedMultiOutputRva),
        static_cast<unsigned long long>(kFullMultiOutputRva));
    return true;
}

bool ResolveStereoData()
{
    if (g_stereoEnableAddress && g_stereoSeparationAddress &&
        g_stereoBaselineAddress && g_stereoConvergenceAddress)
        return true;
    using DisplayGetter = std::uint8_t*(__fastcall*)();
    auto getter = reinterpret_cast<DisplayGetter>(g_exeBase + kDisplayGetterRva);
    std::uint8_t* display{};
    __try
    {
        display = getter();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        display = nullptr;
    }
    if (!display || !IsReadable(display + kStereoEnableOffset, 1) ||
        !IsReadable(display + kStereoObjectOffset + kStereoBaselineOffset, sizeof(float)) ||
        !IsReadable(display + kStereoObjectOffset + kStereoConvergenceOffset, sizeof(float)))
        return false;
    g_stereoEnableAddress = display + kStereoEnableOffset;
    g_stereoBaselineAddress = reinterpret_cast<float*>(
        display + kStereoObjectOffset + kStereoBaselineOffset);
    g_stereoScaleAddress = reinterpret_cast<float*>(
        display + kStereoObjectOffset + kStereoScaleOffset);
    g_stereoSeparationAddress = reinterpret_cast<float*>(
        display + kStereoObjectOffset + kStereoSeparationOffset);
    g_stereoConvergenceAddress = reinterpret_cast<float*>(
        display + kStereoObjectOffset + kStereoConvergenceOffset);
    Log("display resolved=%p gate=%p baseline=%p scale=%p separation=%p convergence=%p "
        "values=%.6f/%.6f/%.6f/%.6f",
        display, g_stereoEnableAddress, g_stereoBaselineAddress, g_stereoScaleAddress,
        g_stereoSeparationAddress, g_stereoConvergenceAddress,
        *g_stereoBaselineAddress, *g_stereoScaleAddress,
        *g_stereoSeparationAddress, *g_stereoConvergenceAddress);
    return true;
}

bool ApplyCamFixSite(std::size_t index, bool disable)
{
    if (index >= kCamFixSites.size())
        return false;
    if (g_camFixSiteApplied[index].load(std::memory_order_acquire) == disable)
        return true;
    const auto site = g_exeBase + kCamFixSites[index];
    const auto size = kCamFixSizes[index];
    if (!IsReadable(reinterpret_cast<void*>(site), size))
        return false;
    if (!g_camFixCaptured)
    {
        if (index < kCamFixExpected.size() &&
            std::memcmp(reinterpret_cast<void*>(site), kCamFixExpected[index].data(),
                kCamFixExpected[index].size()) != 0)
        {
            Log("[CAMFIX] site %zu byte mismatch - not patching", index);
            return false;
        }
        std::memcpy(g_camFixOriginal[index].data(), reinterpret_cast<void*>(site), size);
    }
    DWORD oldProtect{};
    if (!VirtualProtect(reinterpret_cast<void*>(site), size,
            PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    if (disable)
        std::memset(reinterpret_cast<void*>(site), 0x90, size);
    else
        std::memcpy(reinterpret_cast<void*>(site), g_camFixOriginal[index].data(), size);
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(site), size, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), size);
    g_camFixSiteApplied[index].store(disable, std::memory_order_release);
    return true;
}

bool ApplyCamFix(bool enable)
{
    // Block ONLY the eye-0 setter pair (sites 0 and 1).  Eye 0's block is what
    // gameplay projects with, so blocking it restores interaction; leaving the
    // eye-1 pair (sites 2 and 3) and the flag store running keeps every
    // per-frame side effect the cinematic system depends on.  Blocking all four
    // fixed interaction but stalled cutscenes.
    bool ok = true;
    for (std::size_t i = 0; i < 2; ++i)
        ok = ApplyCamFixSite(i, enable) && ok;
    // Capture is complete once every site has been read once.
    if (!g_camFixCaptured)
    {
        ApplyCamFixSite(kCamFixFlagIndex,
            g_camFlagStoreDisabled.load(std::memory_order_acquire));
        g_camFixCaptured = true;
    }
    Log("[CAMFIX] shared-camera eye-matrix writes %s; +0x4F5 handshake store %s",
        enable ? "DISABLED (interaction fix on)" : "restored",
        g_camFlagStoreDisabled.load(std::memory_order_acquire) ? "DISABLED" : "active");
    return ok;
}

MultiOutputCameraJobFn g_realCameraJob{};

struct CamViewCacheSnapshot
{
    std::uint8_t* buffer{};
    std::uint32_t* flags{};
    std::uint32_t* currentIndex{};
    std::uint32_t entries{};
    std::uint32_t savedCurrent{};
    std::uint8_t data[kBlockMaxEntries * kBlockEntryStride]{};
    std::uint32_t savedFlags[kBlockMaxEntries]{};
    bool valid{};
};

void SnapshotCamViewCache(std::uint8_t* block, CamViewCacheSnapshot& snapshot)
{
    snapshot.valid = false;
    __try
    {
        if (!IsReadable(block, 0x28))
            return;
        auto* buffer = *reinterpret_cast<std::uint8_t**>(block + kBlockBufferPtr);
        auto* flags = *reinterpret_cast<std::uint32_t**>(block + kBlockFlagsPtr);
        const auto count = *reinterpret_cast<std::uint32_t*>(block + kBlockCount);
        auto* current = reinterpret_cast<std::uint32_t*>(block + kBlockCurrentIndex);
        if (!buffer || count == 0 || count > kBlockMaxEntries)
            return;
        const std::size_t span = static_cast<std::size_t>(count) * kBlockEntryStride;
        if (!IsReadable(buffer, span))
            return;
        std::memcpy(snapshot.data, buffer, span);
        if (flags && IsReadable(flags, count * sizeof(std::uint32_t)))
        {
            std::memcpy(snapshot.savedFlags, flags, count * sizeof(std::uint32_t));
            snapshot.flags = flags;
        }
        snapshot.buffer = buffer;
        snapshot.entries = count;
        snapshot.currentIndex = current;
        snapshot.savedCurrent = *current;
        snapshot.valid = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        snapshot.valid = false;
    }
}

void RestoreCamViewCache(const CamViewCacheSnapshot& snapshot)
{
    if (!snapshot.valid)
        return;
    __try
    {
        const std::size_t span =
            static_cast<std::size_t>(snapshot.entries) * kBlockEntryStride;
        std::memcpy(snapshot.buffer, snapshot.data, span);
        if (snapshot.flags)
            std::memcpy(snapshot.flags, snapshot.savedFlags,
                snapshot.entries * sizeof(std::uint32_t));
        if (snapshot.currentIndex)
            *snapshot.currentIndex = snapshot.savedCurrent;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// [CAMJOBSKIP] DIAGNOSTIC (not a shipping behaviour): skip the substituted
// camera job entirely while stereo is on.  With the job skipped the left eye
// is black and conversations still freeze, so the camera job is not the
// cause of the freeze.  Never ship this enabled.
constexpr bool kSkipCameraJobInStereo = false;

void InvokeTrackedCameraJob(void* camera)
{
    BeginSepProbeCameraJob(camera);
    g_realCameraJob(camera);
    // The sink drive must come AFTER the game's camera job.  Driving it
    // before (log-proven) meant the job overwrote the field
    // every frame - drive lines showed the mod's 1.55 replaced by the game's 0.79
    // on every sample - so the write never survived to the screen, first
    // person stayed narrow, and third person stayed broken after leaving it.
    DriveFovSink(camera);
    EndSepProbeCameraJob();
}

void CallDeferredCameraJob(void* camera)
{
    if (!g_stereoEnabled.load(std::memory_order_acquire) || !camera)
    {
        InvokeTrackedCameraJob(camera);
        return;
    }

    std::uint8_t cameraReady{};
    __try
    {
        cameraReady = *(static_cast<std::uint8_t*>(camera) + kCamFlagByteA);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InvokeTrackedCameraJob(camera);
        return;
    }

    if (ReadTrueOutput() == 0 && cameraReady == 0)
    {
        const auto deferred = g_leftCameraDeferred.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (deferred <= 8)
            Log("[CAMDEFER] skipped incomplete output0 camera=%p ready=0", camera);
        return;
    }

    InvokeTrackedCameraJob(camera);
}
extern "C" void __fastcall RetailCameraJobPolicy(void* camera)
{
    if (!g_realCameraJob)
        return;
    if constexpr (kSkipCameraJobInStereo)
    {
        if (g_stereoEnabled.load(std::memory_order_acquire))
        {
            g_camWrapCalls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_camWrapCalls.fetch_add(1, std::memory_order_relaxed);
    if (!camera)
    {
        CallDeferredCameraJob(camera);
        return;
    }
    if (!g_camVtableLogged.exchange(true, std::memory_order_acq_rel))
    {
        // Identify the shared-camera setters the job calls (vtable slots +8 and
        // +0x10) so their write targets can be found offline.
        __try
        {
            const auto vtable = *reinterpret_cast<std::uintptr_t*>(camera);
            const auto asRva = [](std::uintptr_t value) -> unsigned long long {
                return value >= g_exeBase && value < g_exeBase + kRetailImageSize
                    ? static_cast<unsigned long long>(value - g_exeBase) : 0ull;
            };
            const auto* slots = reinterpret_cast<std::uintptr_t*>(vtable);
            Log("[CAMWRAP] camera=%p vtableRva=+0x%llX setView(+8)=+0x%llX "
                "setProj(+0x10)=+0x%llX getter(+0x58)=+0x%llX",
                camera, asRva(vtable), asRva(slots[1]), asRva(slots[2]),
                asRva(slots[11]));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    // Snapshot the EXTERNAL view caches the setters actually write through,
    // for every per-eye proj/view block on this camera.
    if (!g_camWrapEnabled.load(std::memory_order_acquire))
    {
        CallDeferredCameraJob(camera);
        return;
    }
    auto* bytes = static_cast<std::uint8_t*>(camera);
    CamViewCacheSnapshot snapshots[kCamViewBlocks.size()]{};
    for (std::size_t i = 0; i < kCamViewBlocks.size(); ++i)
        SnapshotCamViewCache(bytes + kCamViewBlocks[i], snapshots[i]);

    CallDeferredCameraJob(camera);

    std::uint32_t restored = 0;
    for (auto& snapshot : snapshots)
    {
        if (!snapshot.valid)
            continue;
        RestoreCamViewCache(snapshot);
        ++restored;
    }
    if (restored)
        g_camWrapRestored.fetch_add(1, std::memory_order_relaxed);
    if (!g_camWrapDiffLogged.exchange(true, std::memory_order_acq_rel))
        Log("[CAMWRAP] view-cache restore active: %u/%zu blocks snapshotted "
            "(entries %u/%u/%u/%u)", restored, kCamViewBlocks.size(),
            snapshots[0].entries, snapshots[1].entries,
            snapshots[2].entries, snapshots[3].entries);
}

// [SAMPLE] Sampling profiler that assumes nothing about where the state
// lives: periodically sample
// the instruction pointer of the busiest game threads and bucket it by code
// address.  Run it while frozen in stereo, then while the same scene runs in
// mono, and diff the two histograms - code that appears in the mono window and
// is absent in the stereo window is the work that stopped happening.  That
// names the stalled subsystem without knowing a single offset.
// Round-robin across all ~236 threads with a CPU-delta filter yields ~10
// samples per 5s window, so instead the threads the game actually runs the
// mod's hooks on - its real work threads - are remembered and sampled hard.
constexpr std::size_t kHotThreadSlots = 8;
std::array<std::atomic<DWORD>, kHotThreadSlots> g_hotThreads{};

void RecordHotThread()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const auto id = GetCurrentThreadId();
    for (auto& slot : g_hotThreads)
    {
        const auto current = slot.load(std::memory_order_acquire);
        if (current == id)
            return;
        if (current == 0)
        {
            DWORD expected = 0;
            if (slot.compare_exchange_strong(expected, id,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return;
        }
    }
}

constexpr std::size_t kSampleSlots = 2048;
struct SampleSlot
{
    std::atomic<std::uint32_t> key{};      // (rva >> 6) + 1, 0 = empty
    std::atomic<std::uint32_t> count{};
};
std::array<SampleSlot, kSampleSlots> g_sampleSlots{};
std::atomic_bool g_sampleRun{};
std::atomic<std::uint64_t> g_sampleTotal{};
HANDLE g_sampleThreadHandle{};

// [STACK] RIP sampling cannot resolve this (one NPC's animation is below the
// noise floor).  Stacks are different in kind: a
// leaf address says where a thread is, a stack says what it is waiting ON.  x64
// PEs carry unwind data, so full stacks are recoverable without symbols via
// RtlLookupFunctionEntry + RtlVirtualUnwind.  Capture stacks from the game's
// work threads, bucket identical ones, and compare the frozen conversation
// against the working one.
constexpr std::size_t kStackFrames = 12;
constexpr std::size_t kStackBuckets = 48;
struct StackBucket
{
    std::atomic<std::uint64_t> hash{};
    std::atomic<std::uint32_t> count{};
    std::uint32_t frames[kStackFrames]{};
    std::uint32_t depth{};
};
std::array<StackBucket, kStackBuckets> g_stackBuckets{};

void StackRecord(const std::uint32_t* frames, std::uint32_t depth)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < depth; ++i)
        hash = (hash ^ frames[i]) * 1099511628211ull;
    if (!hash)
        hash = 1;
    const auto start = static_cast<std::size_t>(hash) % kStackBuckets;
    for (std::size_t probe = 0; probe < kStackBuckets; ++probe)
    {
        auto& bucket = g_stackBuckets[(start + probe) % kStackBuckets];
        auto current = bucket.hash.load(std::memory_order_acquire);
        if (current == 0)
        {
            std::uint64_t expected = 0;
            if (bucket.hash.compare_exchange_strong(expected, hash,
                    std::memory_order_acq_rel, std::memory_order_acquire))
            {
                for (std::uint32_t i = 0; i < depth; ++i)
                    bucket.frames[i] = frames[i];
                bucket.depth = depth;
                bucket.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            current = bucket.hash.load(std::memory_order_acquire);
        }
        if (current == hash)
        {
            bucket.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void StackCapture(CONTEXT& context)
{
    std::uint32_t frames[kStackFrames]{};
    std::uint32_t depth = 0;
    for (std::size_t i = 0; i < kStackFrames; ++i)
    {
        const auto rip = static_cast<std::uintptr_t>(context.Rip);
        if (!rip)
            break;
        if (rip >= g_exeBase && rip < g_exeBase + kRetailImageSize)
            frames[depth++] = static_cast<std::uint32_t>(rip - g_exeBase);
        DWORD64 imageBase = 0;
        auto* functionEntry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!functionEntry)
        {
            // Leaf function: return address sits at the top of the stack.
            if (!IsReadable(reinterpret_cast<void*>(context.Rsp), sizeof(DWORD64)))
                break;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, functionEntry,
            &context, &handlerData, &establisherFrame, nullptr);
    }
    if (depth >= 2)
        StackRecord(frames, depth);
}

void StackReport(bool stereo)
{
    for (int rank = 0; rank < 6; ++rank)
    {
        std::uint32_t bestCount = 0;
        std::size_t best = kStackBuckets;
        for (std::size_t i = 0; i < kStackBuckets; ++i)
        {
            const auto count = g_stackBuckets[i].count.load(std::memory_order_relaxed);
            if (g_stackBuckets[i].hash.load(std::memory_order_relaxed) && count > bestCount)
            {
                bestCount = count;
                best = i;
            }
        }
        if (best == kStackBuckets || bestCount == 0)
            break;
        char line[512]{};
        std::size_t used = 0;
        for (std::uint32_t i = 0; i < g_stackBuckets[best].depth && used + 12 < sizeof(line); ++i)
        {
            StringCchPrintfA(line + used, 12, "%X<", g_stackBuckets[best].frames[i]);
            used = std::strlen(line);
        }
        Log("[STACK] stereo=%d #%d n=%u %s", stereo ? 1 : 0, rank + 1, bestCount, line);
        g_stackBuckets[best].count.store(0, std::memory_order_relaxed);
    }
    for (auto& bucket : g_stackBuckets)
    {
        bucket.hash.store(0, std::memory_order_relaxed);
        bucket.count.store(0, std::memory_order_relaxed);
        bucket.depth = 0;
    }
}

void SampleRecord(std::uint32_t rva)
{
    const std::uint32_t key = (rva >> 6) + 1;
    auto start = static_cast<std::size_t>((key * 2654435761u) & (kSampleSlots - 1));
    for (std::size_t probe = 0; probe < 64; ++probe)
    {
        auto& slot = g_sampleSlots[(start + probe) & (kSampleSlots - 1)];
        auto current = slot.key.load(std::memory_order_acquire);
        if (current == 0)
        {
            std::uint32_t expected = 0;
            if (!slot.key.compare_exchange_strong(expected, key,
                    std::memory_order_acq_rel, std::memory_order_acquire) &&
                slot.key.load(std::memory_order_acquire) != key)
                continue;
        }
        else if (current != key)
            continue;
        slot.count.fetch_add(1, std::memory_order_relaxed);
        g_sampleTotal.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

void SampleReport(bool stereo)
{
    // Top buckets, then clear for the next window.
    for (int rank = 0; rank < 18; ++rank)
    {
        std::uint32_t bestCount = 0;
        std::size_t bestIndex = kSampleSlots;
        for (std::size_t i = 0; i < kSampleSlots; ++i)
        {
            const auto count = g_sampleSlots[i].count.load(std::memory_order_relaxed);
            if (g_sampleSlots[i].key.load(std::memory_order_relaxed) && count > bestCount)
            {
                bestCount = count;
                bestIndex = i;
            }
        }
        if (bestIndex == kSampleSlots || bestCount == 0)
            break;
        const auto key = g_sampleSlots[bestIndex].key.load(std::memory_order_relaxed);
        Log("[SAMPLE] stereo=%d #%d rva=+0x%llX hits=%u", stereo ? 1 : 0, rank + 1,
            static_cast<unsigned long long>(key - 1) << 6, bestCount);
        g_sampleSlots[bestIndex].count.store(0, std::memory_order_relaxed);
    }
    for (auto& slot : g_sampleSlots)
    {
        slot.key.store(0, std::memory_order_relaxed);
        slot.count.store(0, std::memory_order_relaxed);
    }
    Log("[SAMPLE] stereo=%d window total=%llu", stereo ? 1 : 0,
        static_cast<unsigned long long>(g_sampleTotal.exchange(0, std::memory_order_relaxed)));
}

DWORD WINAPI SampleThreadProc(LPVOID)
{
    const DWORD self = GetCurrentThreadId();
    HANDLE handles[kHotThreadSlots]{};
    DWORD handleIds[kHotThreadSlots]{};
    DWORD lastReport = GetTickCount();
    while (g_sampleRun.load(std::memory_order_acquire))
    {
        for (std::size_t i = 0; i < kHotThreadSlots; ++i)
        {
            const auto id = g_hotThreads[i].load(std::memory_order_acquire);
            if (!id || id == self)
                continue;
            if (handleIds[i] != id)
            {
                if (handles[i])
                    CloseHandle(handles[i]);
                handles[i] = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                    FALSE, id);
                handleIds[i] = id;
            }
            if (!handles[i])
                continue;
            if (SuspendThread(handles[i]) == static_cast<DWORD>(-1))
                continue;
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            const bool ok = GetThreadContext(handles[i], &context) != 0;
            ResumeThread(handles[i]);
            if (ok)
            {
                __try
                {
                    StackCapture(context);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
        }
        const auto now = GetTickCount();
        if (now - lastReport >= 5000)
        {
            lastReport = now;
            StackReport(g_stereoEnabled.load(std::memory_order_acquire));
        }
        Sleep(4);
    }
    for (auto& handle : handles)
    {
        if (handle)
            CloseHandle(handle);
    }
    return 0;
}

extern "C" void __fastcall RetailUpdateDispatchPolicy(void* manager, void* context)
{
    const auto mode = g_stereoEnabled.load(std::memory_order_relaxed) ? 1u : 0u;
    g_updateDispatchCalls[mode].fetch_add(1, std::memory_order_relaxed);
    g_updateDispatchArg0.store(reinterpret_cast<std::uintptr_t>(manager),
        std::memory_order_release);
    g_updateDispatchArg1.store(reinterpret_cast<std::uintptr_t>(context),
        std::memory_order_release);
    if (g_realUpdateDispatch)
        g_realUpdateDispatch(manager, context);
}

LONG CALLBACK GateWatchHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    auto* context = info->ContextRecord;
    if (!context || !(context->Dr6 & 0xF))
        return EXCEPTION_CONTINUE_SEARCH;
    context->Dr6 = 0;
    const auto rip = static_cast<std::uintptr_t>(context->Rip);
    const auto rva = rip >= g_exeBase && rip < g_exeBase + kRetailImageSize
        ? rip - g_exeBase : 0;
    g_gateWatchHits.fetch_add(1, std::memory_order_relaxed);
    if (rva)
    {
        for (auto& slot : g_gateWatchReaders)
        {
            auto existing = slot.load(std::memory_order_acquire);
            if (existing == rva)
                break;
            if (existing == 0 && slot.compare_exchange_strong(existing, rva,
                    std::memory_order_acq_rel))
            {
                Log("[GATEWATCH] new reader +0x%llX",
                    static_cast<unsigned long long>(rva));
                break;
            }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ApplyGateWatchToThreads(bool arm)
{
    // The stereo ENABLE byte (display+0x244).  Code that only checks stereo
    // DURING a conversation is only seen if the watch stays armed through the
    // frozen scene, so it does.
    if (!g_stereoEnableAddress)
        return;
    const auto address = reinterpret_cast<std::uintptr_t>(g_stereoEnableAddress);
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto self = GetCurrentThreadId();
    const auto process = GetCurrentProcessId();
    std::uint32_t applied = 0;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                continue;
            const auto thread = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            DebugRegisterUpdates::Guard registerUpdate;
            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context))
                {
                    if (arm)
                    {
                        context.Dr0 = address;
                        // local enable DR0, RW=11b (read/write), LEN=00b (1 byte)
                        context.Dr7 = (context.Dr7 & ~0xF0000Full) | 0x1ull |
                            (0x3ull << 16);
                    }
                    else
                    {
                        context.Dr0 = 0;
                        context.Dr7 &= ~0xF0000Full;
                    }
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (SetThreadContext(thread, &context))
                        ++applied;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    Log("[GATEWATCH] %s on %u threads (outputCount=%p)", arm ? "ARMED" : "disarmed",
        applied, reinterpret_cast<void*>(address));
}

// [SPLIT] User eye separation for native stereo, engine centimetres.  Zero
// means the stock value (6.0 = 60 mm) the engine shipped with.
std::atomic<float> g_nativeBaselineCm{0.0f};

// [SKEWZERO] The engine's own "stereo" moves no camera at all.  Measured:
// the right pass gets projection skew P[2][0] = baseline / scale
// (6.0 / 115.118 = 0.0521) and neither eye gets a view translation - a flat
// sideways image shift, not depth.  All real depth comes from the mod's
// camera offsets (right-eye seat + [SPLITSYM]).  The skew is therefore
// removed, not held: scale is set so baseline / scale is negligible.  Both
// passes then render parallel, unskewed projections, exactly the geometry
// AER renders, so the honest per-eye lens claim is exact and the native
// Convergence slider (a tangent shift at submit) is the only convergence.
constexpr float kNativeResidualSkew = 1.0e-5f;

void WriteNativeBaseline(float centimetres)
{
    if (!g_stereoBaselineAddress)
        return;
    __try
    {
        *g_stereoBaselineAddress = centimetres;
        if (g_stereoScaleAddress && centimetres > 1e-3f)
            *g_stereoScaleAddress = centimetres / kNativeResidualSkew;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void SetStereo(bool enable)
{
    if (!g_enginePatchesReady.load(std::memory_order_acquire) || !ResolveStereoData())
    {
        Log("toggle rejected: engine patches or display are not ready");
        Beep(300, 180);
        return;
    }
    if (enable)
        g_stereoEverEnabled.store(true, std::memory_order_release);
    if (enable)
    {
        RetailXr::SetMonoSubmit(false);   // [MONOVR]
        if (g_aerEnabled.load(std::memory_order_acquire))
        {
            g_vrModeSwitching.store(true, std::memory_order_release);   // [MODETRACK2]
            SetAer(false);
        }
        if (!g_savedStereoData)
        {
            g_savedStereoEnable = *g_stereoEnableAddress;
            g_savedStereoSeparation = *g_stereoSeparationAddress;
            g_savedStereoBaseline = *g_stereoBaselineAddress;
            g_savedStereoScale = g_stereoScaleAddress ? *g_stereoScaleAddress : 0.0f;
            g_savedStereoConvergence = *g_stereoConvergenceAddress;
            g_savedStereoData = true;
        }
        // Preserve the stock, positive separation input. The physical baseline
        // is the only value F10 changes.
        *g_stereoSeparationAddress = g_savedStereoSeparation;
        *g_stereoBaselineAddress = g_savedStereoBaseline;
        {
            const float custom = g_nativeBaselineCm.load(std::memory_order_relaxed);
            WriteNativeBaseline(custom > 0.0f ? custom : g_savedStereoBaseline);
        }
        *g_stereoEnableAddress = 1;
        RetailXr::ResetHeadPoseCenter();
        g_stereoEnabled.store(true, std::memory_order_release);
        g_vrModeSwitching.store(false, std::memory_order_release);
        if (g_aerEnabled.exchange(false, std::memory_order_acq_rel))
            Log("[AER] disabled because legacy native SBS was enabled");
        StartDynamicHeadTracking();
        // [NATIVEFOV] The pinned 87-degree lens lives on this breakpoint; only
        // AER ever armed it, so native stereo rendered the stock 48 degrees.
        SetFovSetterBreakpointAllThreads(true);
        // Reset the counters of every DR2/DR3 diagnostic ([MOTIONGATE],
        // [AIGRAPH], [AIMODE], [TRAYTICK], [ACTUPD], [AICOMP], [CANAPPLY],
        // [MODESW], [TASKSKIP]/[ACTRATE]).
        {
            for (auto& counter : g_motionGateHits)
                for (auto& c : counter)
                    c.store(0, std::memory_order_relaxed);
            g_motionGateOffPresent.store(0, std::memory_order_release);
            // [AIGRAPH]
            g_aiHits[0].store(0, std::memory_order_relaxed);
            g_aiHits[1].store(0, std::memory_order_relaxed);
            g_aiOverflow.store(0, std::memory_order_relaxed);
            g_aiGraphOffPresent.store(0, std::memory_order_release);
            for (auto& s : g_aiNodes) s.key.store(0, std::memory_order_relaxed);
            for (auto& s : g_aiConds) s.key.store(0, std::memory_order_relaxed);
            // [AIMODE]
            g_traceWritten.store(0, std::memory_order_release);
            g_traceLogged.store(0, std::memory_order_relaxed);
            g_traceHits[0].store(0, std::memory_order_relaxed);
            g_traceHits[1].store(0, std::memory_order_relaxed);
            g_aiModeComponent.store(0, std::memory_order_relaxed);
            g_aiModePendingAddr.store(0, std::memory_order_relaxed);
            g_aiModeOffPresent.store(0, std::memory_order_release);
            // [AIGRAPH] v2
            g_aiHits[0].store(0, std::memory_order_relaxed);
            g_aiHits[1].store(0, std::memory_order_relaxed);
            g_aiOverflow.store(0, std::memory_order_relaxed);
            g_aiGraphOffPresent.store(0, std::memory_order_release);
            for (auto& s : g_aiNodes) s.key.store(0, std::memory_order_relaxed);
            for (auto& s : g_aiConds) s.key.store(0, std::memory_order_relaxed);
            // [TRAYTICK]
            g_traceWritten.store(0, std::memory_order_release);
            g_traceLogged.store(0, std::memory_order_relaxed);
            g_trayStacksBefore.store(0, std::memory_order_relaxed);
            g_trayStacksAfter.store(0, std::memory_order_relaxed);
            g_trayOffPresent.store(0, std::memory_order_release);
            for (auto& s : g_trays) s.tray.store(0, std::memory_order_relaxed);
            for (auto& k : g_trayHits) for (auto& c : k) c.store(0, std::memory_order_relaxed);
            // [ACTUPD]
            g_actOffPresent.store(0, std::memory_order_release);
            g_actOverflow.store(0, std::memory_order_relaxed);
            for (auto& s : g_acts)
            {
                s.actor.store(0, std::memory_order_relaxed);
                for (int ph = 0; ph < 2; ++ph)
                {
                    s.calls[ph].store(0, std::memory_order_relaxed);
                    s.f1Zero[ph].store(0, std::memory_order_relaxed);
                    s.f2Set[ph].store(0, std::memory_order_relaxed);
                    s.ctlHits[ph].store(0, std::memory_order_relaxed);
                    s.ctlSkip[ph].store(0, std::memory_order_relaxed);
                    s.noCtl[ph].store(0, std::memory_order_relaxed);
                }
            }
            // [AICOMP]
            g_compOffPresent.store(0, std::memory_order_release);
            g_compOverflow.store(0, std::memory_order_relaxed);
            for (auto& s : g_comps)
            {
                s.comp.store(0, std::memory_order_relaxed);
                for (int ph = 0; ph < 2; ++ph)
                {
                    s.calls[ph].store(0, std::memory_order_relaxed);
                    s.state0[ph].store(0, std::memory_order_relaxed);
                    s.state3[ph].store(0, std::memory_order_relaxed);
                    s.stateOther[ph].store(0, std::memory_order_relaxed);
                    s.dtZero[ph].store(0, std::memory_order_relaxed);
                    s.gateHits[ph].store(0, std::memory_order_relaxed);
                    s.gateFalse[ph].store(0, std::memory_order_relaxed);
                    s.holderPaused[ph].store(0, std::memory_order_relaxed);
                }
            }
            // [CANAPPLY]
            g_applyOffPresent.store(0, std::memory_order_release);
            g_applyOverflow.store(0, std::memory_order_relaxed);
            for (auto& s : g_apply)
            {
                s.comp.store(0, std::memory_order_relaxed);
                for (int ph = 0; ph < 2; ++ph)
                {
                    s.calls[ph].store(0, std::memory_order_relaxed);
                    s.refused[ph].store(0, std::memory_order_relaxed);
                    s.blocked[ph].store(0, std::memory_order_relaxed);
                    s.notInterruptible[ph].store(0, std::memory_order_relaxed);
                    s.waitHits[ph].store(0, std::memory_order_relaxed);
                    s.waitTrue[ph].store(0, std::memory_order_relaxed);
                }
            }
            // [MODESW]
            g_modeSwOffPresent.store(0, std::memory_order_release);
            g_modeSwOverflow.store(0, std::memory_order_relaxed);
            for (auto& s : g_holders)
            {
                s.holder.store(0, std::memory_order_relaxed);
                for (int ph = 0; ph < 2; ++ph)
                {
                    s.ticks[ph].store(0, std::memory_order_relaxed);
                    s.differs[ph].store(0, std::memory_order_relaxed);
                    s.ctlRunning[ph].store(0, std::memory_order_relaxed);
                    s.ctlNull[ph].store(0, std::memory_order_relaxed);
                    s.ctlEmpty[ph].store(0, std::memory_order_relaxed);
                    s.lastCtlCount[ph].store(0, std::memory_order_relaxed);
                    s.trayCreates[ph].store(0, std::memory_order_relaxed);
                    s.trayFails[ph].store(0, std::memory_order_relaxed);
                }
            }
            // [TASKSKIP] on DR2, [ACTRATE] on DR3.
            g_taskSkipOffPresent.store(0, std::memory_order_release);
            g_taskSkipArmPresent.store(g_presentCount.load(std::memory_order_relaxed), std::memory_order_release);
            g_rateOverflow.store(0, std::memory_order_relaxed);
            g_hopOverflow.store(0, std::memory_order_relaxed);
            g_gateOverflow.store(0, std::memory_order_relaxed);
            for (int k = 0; k < 2; ++k)
            {
                g_gateAllHits[k].store(0, std::memory_order_relaxed);
                g_gateAllRestricted[k].store(0, std::memory_order_relaxed);
                g_gateAllDropped[k].store(0, std::memory_order_relaxed);
            }
            for (auto& gs : g_gates)
            {
                gs.task.store(0, std::memory_order_relaxed);
                for (int k = 0; k < 2; ++k)
                {
                    gs.hits[k].store(0, std::memory_order_relaxed);
                    gs.restricted[k].store(0, std::memory_order_relaxed);
                    gs.dropped[k].store(0, std::memory_order_relaxed);
                    gs.loopPath[k].store(0, std::memory_order_relaxed);
                }
            }
            for (auto& hs : g_hops)
            {
                hs.task.store(0, std::memory_order_relaxed);
                for (int ph = 0; ph < 2; ++ph)
                {
                    hs.hits[ph].store(0, std::memory_order_relaxed);
                    hs.dtZero[ph].store(0, std::memory_order_relaxed);
                }
            }
            for (auto& s : g_rates)
            {
                s.actor.store(0, std::memory_order_relaxed);
                for (int ph = 0; ph < 2; ++ph)
                {
                    s.updates[ph].store(0, std::memory_order_relaxed);
                    s.firstPresent[ph].store(0, std::memory_order_relaxed);
                    s.lastPresent[ph].store(0, std::memory_order_relaxed);
                }
            }
            g_taskOverflow.store(0, std::memory_order_relaxed);
            g_traceWritten.store(0, std::memory_order_release);
            g_traceLogged.store(0, std::memory_order_relaxed);
            g_taskStacks[0].store(0, std::memory_order_relaxed);
            g_taskStacks[1].store(0, std::memory_order_relaxed);
            for (auto& s : g_tasks)
            {
                s.task.store(0, std::memory_order_relaxed);
                for (int ph = 0; ph < 2; ++ph)
                {
                    s.runs[ph].store(0, std::memory_order_relaxed);
                    s.skips[ph].store(0, std::memory_order_relaxed);
                    s.skipOnce[ph].store(0, std::memory_order_relaxed);
                    s.runNow[ph].store(0, std::memory_order_relaxed);
                    s.maxInterval[ph].store(0, std::memory_order_relaxed);
                }
            }
            for (std::size_t i = 0; i < kCallbackSlots; ++i)
            {
                g_taskCallbacks[i].store(0, std::memory_order_relaxed);
                g_taskCallbackHits[i].store(0, std::memory_order_relaxed);
            }
            // Hang diagnostics are OFF in playable builds.
            if constexpr (kArmHangDiagnostics)
                if (const auto worker = CreateThread(nullptr, 0, &TaskSkipArmThread, nullptr, 0, nullptr))
                    CloseHandle(worker);
        }
        g_viewCbPayloadDumped.store(false, std::memory_order_release);
        g_viewCbPayloadDumpAtPresent.store(
            g_presentCount.load(std::memory_order_relaxed) + 600,
            std::memory_order_release);
        g_camDumpGeneration.fetch_add(1, std::memory_order_acq_rel);
        Log("STEREO ON: gate 0x%02X->1, separation=%.6f baseline=%.6f "
            "scale=%.6f convergence=%.6f engineSkew=%.7f (stock %.4f)",
            g_savedStereoEnable, *g_stereoSeparationAddress, *g_stereoBaselineAddress,
            *g_stereoScaleAddress, *g_stereoConvergenceAddress,
            *g_stereoBaselineAddress / *g_stereoScaleAddress,
            g_savedStereoScale > 1e-3f ? g_savedStereoBaseline / g_savedStereoScale : 0.0f);
        Beep(1500, 100);
        // Gate-reader census: OFF by default.  The enable byte has 34 readers,
        // all renderer; the output count has none while stereo is on (the
        // engine short-circuits that test).  It sets debug registers on every
        // thread, so it is for deliberate census runs only.
        // Both samplers OFF: RIP sampling is noise-dominated here, and stack
        // sampling gives identical profiles in frozen and working scenes; the
        // sequence-tick instrumentation ([SEQCENSUS]) is the useful view.
        constexpr bool kArmGateWatch = false;
        static std::atomic_bool watchDone{false};
        bool expected = false;
        if (kArmGateWatch && watchDone.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            if (!g_gateWatchHandler)
                g_gateWatchHandler = AddVectoredExceptionHandler(1, &GateWatchHandler);
            g_gateWatchArmedAtMs.store(GetTickCount(), std::memory_order_release);
            g_gateWatchLastSweepMs.store(GetTickCount(), std::memory_order_release);
            ApplyGateWatchToThreads(true);
            g_gateWatchArmed.store(true, std::memory_order_release);
        }
    }
    else
    {
        // [SEQCENSUS] the manual stereo-off is the release event; the census
        // stays armed 90 more presents so the diff can see what completes.
        SeqCensusMarkRelease(g_presentCount.load(std::memory_order_relaxed));
        if (g_taskSkipArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_taskSkipOffPresent.store(now, std::memory_order_release);
            Log("[TASKSKIP] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        if (g_modeSwArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_modeSwOffPresent.store(now, std::memory_order_release);
            Log("[MODESW] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        if (g_applyArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_applyOffPresent.store(now, std::memory_order_release);
            Log("[CANAPPLY] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        if (g_compArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_compOffPresent.store(now, std::memory_order_release);
            Log("[AICOMP] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        if (g_actArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_actOffPresent.store(now, std::memory_order_release);
            Log("[ACTUPD] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        if (g_trayArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_trayOffPresent.store(now, std::memory_order_release);
            Log("[TRAYTICK] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        if (g_aiModeArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_aiModeOffPresent.store(now, std::memory_order_release);
            Log("[AIMODE] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        if (g_aiGraphArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_aiGraphOffPresent.store(now, std::memory_order_release);
            Log("[AIGRAPH] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        if (g_motionGateArmed.load(std::memory_order_acquire))
        {
            const auto now = g_presentCount.load(std::memory_order_relaxed);
            g_motionGateOffPresent.store(now, std::memory_order_release);
            Log("[MOTIONGATE] stereo-off at present=%llu; probe stays armed 300 presents",
                static_cast<unsigned long long>(now));
        }
        RestoreGameFov();
        *g_stereoEnableAddress = g_savedStereoEnable;
        *g_stereoSeparationAddress = g_savedStereoSeparation;
        *g_stereoBaselineAddress = g_savedStereoBaseline;
        if (g_stereoScaleAddress && g_savedStereoScale > 1e-3f)
            *g_stereoScaleAddress = g_savedStereoScale;
        *g_stereoConvergenceAddress = g_savedStereoConvergence;
        g_stereoEnabled.store(false, std::memory_order_release);
        if (!g_aerEnabled.load(std::memory_order_acquire))
        {
            if (!g_vrModeSwitching.load(std::memory_order_acquire))   // [MODETRACK2]
                StopDynamicHeadTracking();
            SetFovSetterBreakpointAllThreads(false);
        }
        g_stereoSuspended.store(false, std::memory_order_release);
        if (g_conversationActive.load(std::memory_order_acquire) == 1)
            g_conversationActive.store(0, std::memory_order_release);
        RetailXr::ResetHeadPoseCenter();
        g_uploadMatricesArmed.store(0, std::memory_order_release);
        g_sepProbePreset.store(-1, std::memory_order_release);
        g_sepProbeDumpAtPresent.store(0, std::memory_order_release);
        g_sepProbeEventRecording.store(false, std::memory_order_release);
        Log("STEREO OFF: gate, separation, baseline, and convergence restored");
        Beep(650, 100);
    }
}

void SetAer(bool enable);
// [MONOVR] head-tracked Mono = the AER pipeline with mono submission.
void SetMono(bool enable)
{
    if (enable)
    {
        RetailXr::SetMonoSubmit(true);
        if (!g_aerEnabled.load(std::memory_order_acquire))
            SetAer(true);
        else
            RetailXr::ResetHeadPoseCenter();
        Log("[MONOVR] Mono ON: one picture to both eyes, head tracked");
    }
    else if (RetailXr::GetMonoSubmit())
    {
        RetailXr::SetMonoSubmit(false);
        if (g_aerEnabled.load(std::memory_order_acquire))
            SetAer(false);
        Log("[MONOVR] Mono OFF");
    }
}

void SetAer(bool enable)
{
    if (!g_enginePatchesReady.load(std::memory_order_acquire))
    {
        Log("[AER] toggle rejected: executable validation/engine patches not ready");
        Beep(300, 180);
        return;
    }
    if (enable)
    {
        if (g_stereoEnabled.load(std::memory_order_acquire))
        {
            g_vrModeSwitching.store(true, std::memory_order_release);   // [MODETRACK2]
            SetStereo(false);
        }
        // [AERPAIRSYNC] The native-stereo vehicle-jitter repair established
        // that the second eye must use the game camera already rendered for
        // the first eye, rather than the newer moving-vehicle camera.  AER's
        // equivalent is EYEHOLD.  Older INI files could leave it disabled,
        // silently restoring the same one-eye shake, so every AER activation
        // reasserts the complete paired-camera and paired-head-pose invariant.
        if (!g_eyeHoldEnabled.exchange(true, std::memory_order_acq_rel))
            Log("[AERPAIRSYNC] restored mandatory camera hold from a legacy disabled setting");
        if (g_aerFreshHeadPose.exchange(false, std::memory_order_acq_rel))
            Log("[AERPAIRSYNC] restored mandatory head-pose hold from a legacy fresh-pose setting");
        DlssUpscaler::SetAerBypass(!RetailXr::GetMonoSubmit());
        g_eyeHoldValid.store(false, std::memory_order_release);
        g_eyeHoldAppliedThisFrame.store(false, std::memory_order_release);
        g_aerEnabled.store(true, std::memory_order_release);
        g_vrModeSwitching.store(false, std::memory_order_release);
        RetailXr::ResetAerEyeSequence();
        RetailXr::ResetHeadPoseCenter();
        StartDynamicHeadTracking();
        SetFovSetterBreakpointAllThreads(true);
        Log("[AER] ON: full-frame alternate-eye rendering; halfEye=%.4fm "
            "convergence=%.4f swap=%d pairCameraSync=1 freshHead=%d",
            g_aerHalfEyeMeters.load(std::memory_order_relaxed),
            RetailXr::GetAerConvergence(),
            g_aerSwapEyes.load(std::memory_order_relaxed) ? 1 : 0,
            g_aerFreshHeadPose.load(std::memory_order_relaxed) ? 1 : 0);
        Beep(1700, 90);
        Beep(2200, 110);
    }
    else
    {
        DlssUpscaler::SetAerBypass(false);
        g_aerEnabled.store(false, std::memory_order_release);
        g_eyeHoldValid.store(false, std::memory_order_release);
        g_eyeHoldAppliedThisFrame.store(false, std::memory_order_release);
        if (!g_stereoEnabled.load(std::memory_order_acquire))
        {
            SetFovSetterBreakpointAllThreads(false);
            if (!g_vrModeSwitching.load(std::memory_order_acquire))   // [MODETRACK2]
                StopDynamicHeadTracking();
        }
        RetailXr::ResetHeadPoseCenter();
        Log("[AER] OFF: mono game render restored");
        Beep(650, 100);
    }
}
void LogPoolCensus(bool stereo)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    static std::atomic_bool legendLogged{false};
    if (!legendLogged.exchange(true, std::memory_order_acq_rel))
    {
        Log("[POOLCEN] legend slots(0-27)=378,3f0,418,440,4b8,4e0,508,530,558,6c0,"
            "760,788,7b0,7d8,800,828,850,8f0,990,ad0,af8,b20,c88,cb0,cd8,d00,d28,d50 "
            "gated(shared,last-eye)=17:8f0,22:c88,23:cb0,24:cd8,25:d00,26:d28,27:d50");
    }

    const auto globalAddr = g_exeBase + kPoolGlobalRva;
    std::uintptr_t poolBase = 0;
    char marks[kPoolSlotOffsets.size() + 1] = {};
    int nonnull = 0;
    bool ok = false;
    __try
    {
        if (IsReadable(reinterpret_cast<void*>(globalAddr), sizeof(std::uintptr_t)))
        {
            poolBase = *reinterpret_cast<std::uintptr_t*>(globalAddr);
            if (poolBase)
            {
                for (std::size_t i = 0; i < kPoolSlotOffsets.size(); ++i)
                {
                    const auto slotAddr = poolBase + kPoolSlotOffsets[i];
                    std::uintptr_t val = 0;
                    if (IsReadable(reinterpret_cast<void*>(slotAddr), sizeof(std::uintptr_t)))
                        val = *reinterpret_cast<std::uintptr_t*>(slotAddr);
                    marks[i] = val ? '1' : '0';
                    if (val)
                        ++nonnull;
                }
                ok = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    if (ok)
        Log("[POOLCEN] stereo=%d base=%p nonnull=%d slots=%s", stereo ? 1 : 0,
            reinterpret_cast<void*>(poolBase), nonnull, marks);
    else
        Log("[POOLCEN] stereo=%d base=%p READ_FAIL", stereo ? 1 : 0,
            reinterpret_cast<void*>(poolBase));
}

const char* LightManagerMode(bool stereo)
{
    if (stereo)
        g_everStereo.store(true, std::memory_order_release);
    return stereo ? "stereo"
        : g_everStereo.load(std::memory_order_acquire) ? "postmono" : "cleanmono";
}

void LogLightManagerSample(bool stereo, bool wantDump)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const char* mode = LightManagerMode(stereo);
    static std::atomic<std::uint32_t> lastModeTag{0xFFFFFFFF};
    const std::uint32_t modeTag = stereo ? 1u : (mode[0] == 'p' ? 2u : 0u);
    const bool modeChanged =
        lastModeTag.exchange(modeTag, std::memory_order_acq_rel) != modeTag;
    const bool dump = wantDump || modeChanged;

    std::uintptr_t ctx = 0, mgr = 0, shapePtr = 0, selected = 0, vtbl = 0;
    std::uint32_t flag = 0xFF;
    std::uint8_t bytes[0x400]{};
    bool haveBytes = false;
    __try
    {
        const auto globalAddr = g_exeBase + kDeferredCtxGlobalRva;
        if (IsReadable(reinterpret_cast<void*>(globalAddr), sizeof(std::uintptr_t)))
            ctx = *reinterpret_cast<std::uintptr_t*>(globalAddr);
        if (ctx && IsReadable(reinterpret_cast<void*>(ctx + kLightManagerCtxOffset),
                sizeof(std::uintptr_t)))
            mgr = *reinterpret_cast<std::uintptr_t*>(ctx + kLightManagerCtxOffset);
        if (mgr && IsReadable(reinterpret_cast<void*>(mgr), sizeof(bytes)))
        {
            std::memcpy(bytes, reinterpret_cast<void*>(mgr), sizeof(bytes));
            haveBytes = true;
            flag = bytes[kLightShapeSelectorFlagOffset];
            std::memcpy(&shapePtr, bytes + kLightShapePointerOffset, sizeof(shapePtr));
            selected = flag == 0 ? shapePtr : mgr + kLightShapeEmbeddedOffset;
            if (selected && IsReadable(reinterpret_cast<void*>(selected),
                    sizeof(std::uintptr_t)))
                vtbl = *reinterpret_cast<std::uintptr_t*>(selected);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        haveBytes = false;
    }
    const auto asRva = [](std::uintptr_t address) -> std::uintptr_t {
        return address >= g_exeBase && address < g_exeBase + 0x8000000
            ? address - g_exeBase : 0;
    };
    Log("[LMOBJ] mode=%s ctx=%p mgr=%p flag168=0x%02X ptr160=%p selected=%p vtblRva=+0x%llX",
        mode, reinterpret_cast<void*>(ctx), reinterpret_cast<void*>(mgr), flag,
        reinterpret_cast<void*>(shapePtr), reinterpret_cast<void*>(selected),
        static_cast<unsigned long long>(asRva(vtbl)));
    if (dump && haveBytes)
    {
        for (std::size_t line = 0; line < sizeof(bytes) / 0x40; ++line)
        {
            char hex[0x40 * 2 + 1]{};
            for (std::size_t i = 0; i < 0x40; ++i)
                StringCchPrintfA(hex + i * 2, 3, "%02X", bytes[line * 0x40 + i]);
            Log("[LMOBJ] mode=%s +%03zX %s", mode, line * 0x40, hex);
        }
    }

    // [PROBEG] caller-gate sampler: which condition stops producer B at F9.
    std::uint32_t viewFlags = 0xFFFFFFFF;
    std::uint8_t force = 0xFF;
    std::uint8_t relightMaster = 0xFF;
    std::uint64_t mask6e0 = 0xFFFFFFFFFFFFFFFFull;
    std::uintptr_t probeCtxLogged = 0;
    std::uintptr_t objAddr[2]{};
    std::uint32_t objCount[2] = {0xFFFFFFFF, 0xFFFFFFFF};
    std::uint8_t g2d8[2] = {0xFF, 0xFF}, g2d9[2] = {0xFF, 0xFF};
    std::uint8_t g6d0[2] = {0xFF, 0xFF}, g6d1[2] = {0xFF, 0xFF};
    __try
    {
        const auto forceAddr = g_exeBase + kProbeForceGlobalRva;
        if (IsReadable(reinterpret_cast<void*>(forceAddr), 1))
            force = *reinterpret_cast<const std::uint8_t*>(forceAddr);
        const auto probeCtx = g_probewCtx.load(std::memory_order_acquire);
        probeCtxLogged = probeCtx;
        if (probeCtx && IsReadable(
                reinterpret_cast<void*>(probeCtx + kProbeViewFlagsOffset), 4))
            viewFlags = *reinterpret_cast<const std::uint32_t*>(
                probeCtx + kProbeViewFlagsOffset);
        if (probeCtx && IsReadable(
                reinterpret_cast<void*>(probeCtx + kFeatureMask64Offset), 8))
            mask6e0 = *reinterpret_cast<const std::uint64_t*>(
                probeCtx + kFeatureMask64Offset);
        if (ctx && IsReadable(
                reinterpret_cast<void*>(ctx + kProbeRelightMasterOffset), 1))
            relightMaster = *reinterpret_cast<const std::uint8_t*>(
                ctx + kProbeRelightMasterOffset);
        for (std::size_t i = 0; i < 2; ++i)
        {
            objAddr[i] = g_probewObjs[i].load(std::memory_order_acquire);
            if (!objAddr[i])
                continue;
            const auto* objBytes = reinterpret_cast<const std::uint8_t*>(objAddr[i]);
            if (IsReadable(objBytes + 0x60, 4))
                objCount[i] = *reinterpret_cast<const std::uint32_t*>(objBytes + 0x60);
            if (IsReadable(objBytes + 0x2D8, 2))
            {
                g2d8[i] = objBytes[0x2D8];
                g2d9[i] = objBytes[0x2D9];
            }
            if (IsReadable(objBytes + 0x6D0, 2))
            {
                g6d0[i] = objBytes[0x6D0];
                g6d1[i] = objBytes[0x6D1];
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    Log("[PROBEG] mode=%s ctx=%p viewFlags=0x%08X bit2=%d force=%u relight=%u "
        "mask6e0=%016llX vfixN=%llu mid=%llu/%llu/%llu "
        "obj0=%p cnt=%u 2d8/9=%u/%u 6d0/1=%u/%u "
        "obj1=%p cnt=%u 2d8/9=%u/%u 6d0/1=%u/%u",
        mode, reinterpret_cast<void*>(probeCtxLogged), viewFlags,
        viewFlags == 0xFFFFFFFF ? -1 : (int)((viewFlags >> 2) & 1),
        force, relightMaster,
        static_cast<unsigned long long>(mask6e0),
        static_cast<unsigned long long>(g_viewFlagsCorrections.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_midFlagsCorrections.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_relightCorrections.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_maskBitCorrections.load(std::memory_order_relaxed)),
        reinterpret_cast<void*>(objAddr[0]), objCount[0], g2d8[0], g2d9[0], g6d0[0], g6d1[0],
        reinterpret_cast<void*>(objAddr[1]), objCount[1], g2d8[1], g2d9[1], g6d0[1], g6d1[1]);
}

struct SepProbeMatrixSnapshot
{
    float matrix[16]{};
    std::uint64_t writes{};
    std::uint32_t threadId{};
    float separation{};
    float baseline{};
};

void ResetSepProbeEvents()
{
    g_sepProbeEventRecording.store(false, std::memory_order_release);
    for (auto& event : g_sepProbeEvents)
        event.ready.store(false, std::memory_order_relaxed);
    g_sepProbeEventNext.store(0, std::memory_order_release);
    g_sepProbeEventGeneration.fetch_add(1, std::memory_order_acq_rel);
    g_sepProbeEventRecording.store(true, std::memory_order_release);
}

void ResetSepProbeCaptures()
{
    for (auto& kind : g_sepProbeMatrices)
    {
        for (auto& slot : kind)
        {
            AcquireSRWLockExclusive(&slot.lock);
            std::memset(slot.matrix, 0, sizeof(slot.matrix));
            slot.writes = 0;
            slot.threadId = 0;
            slot.separation = 0.0f;
            slot.baseline = 0.0f;
            ReleaseSRWLockExclusive(&slot.lock);
        }
    }
}

SepProbeMatrixSnapshot SnapshotSepProbeMatrix(std::size_t kind, std::size_t eye)
{
    SepProbeMatrixSnapshot snapshot{};
    auto& slot = g_sepProbeMatrices[kind][eye];
    AcquireSRWLockExclusive(&slot.lock);
    std::memcpy(snapshot.matrix, slot.matrix, sizeof(snapshot.matrix));
    snapshot.writes = slot.writes;
    snapshot.threadId = slot.threadId;
    snapshot.separation = slot.separation;
    snapshot.baseline = slot.baseline;
    ReleaseSRWLockExclusive(&slot.lock);
    return snapshot;
}

std::uint64_t HashSepProbeMatrix(const float* matrix)
{
    constexpr std::uint64_t kOffset = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffset;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(matrix);
    for (std::size_t i = 0; i < sizeof(float) * 16; ++i)
    {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    return hash;
}

void DumpSepProbeMatrices()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const int preset = g_sepProbePreset.load(std::memory_order_acquire);
    const char* presetName = preset >= 0 &&
        preset < static_cast<int>(kSepProbePresetNames.size())
        ? kSepProbePresetNames[static_cast<std::size_t>(preset)] : "UNKNOWN";
    std::array<std::array<SepProbeMatrixSnapshot, 2>, 2> snapshots{};
    for (std::size_t kind = 0; kind < snapshots.size(); ++kind)
    {
        for (std::size_t eye = 0; eye < snapshots[kind].size(); ++eye)
        {
            auto& snapshot = snapshots[kind][eye];
            snapshot = SnapshotSepProbeMatrix(kind, eye);
            Log("[SEPPROBE] preset=%s kind=%s eye=%zu writes=%llu tid=%u "
                "sep=%.6f baseline=%.6f hash=%016llX "
                "m=[%.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f]",
                presetName, kind == 0 ? "PROJ" : "VIEW", eye,
                static_cast<unsigned long long>(snapshot.writes), snapshot.threadId,
                snapshot.separation, snapshot.baseline,
                static_cast<unsigned long long>(HashSepProbeMatrix(snapshot.matrix)),
                snapshot.matrix[0], snapshot.matrix[1], snapshot.matrix[2], snapshot.matrix[3],
                snapshot.matrix[4], snapshot.matrix[5], snapshot.matrix[6], snapshot.matrix[7],
                snapshot.matrix[8], snapshot.matrix[9], snapshot.matrix[10], snapshot.matrix[11],
                snapshot.matrix[12], snapshot.matrix[13], snapshot.matrix[14], snapshot.matrix[15]);
        }
        float maxDelta = 0.0f;
        for (std::size_t i = 0; i < 16; ++i)
        {
            const float raw = snapshots[kind][0].matrix[i] - snapshots[kind][1].matrix[i];
            const float delta = raw < 0.0f ? -raw : raw;
            if (delta > maxDelta)
                maxDelta = delta;
        }
        Log("[SEPPROBE] preset=%s pair kind=%s eye0Writes=%llu eye1Writes=%llu maxAbsDelta=%.9f",
            presetName, kind == 0 ? "PROJ" : "VIEW",
            static_cast<unsigned long long>(snapshots[kind][0].writes),
            static_cast<unsigned long long>(snapshots[kind][1].writes), maxDelta);
    }

    g_sepProbeEventRecording.store(false, std::memory_order_release);
    const auto generation = g_sepProbeEventGeneration.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < g_sepProbeEvents.size(); ++index)
    {
        const auto& event = g_sepProbeEvents[index];
        if (!event.ready.load(std::memory_order_acquire) ||
            event.generation != generation)
            continue;
        const char* type = event.type == 0 ? "ENTRY" :
            event.type == 1 ? "PROJ" : event.type == 2 ? "VIEW" : "EXIT";
        Log("[SEPJOB] preset=%s event=%zu type=%s job=%llu present=%llu camera=%p "
            "tls=%d tid=%u sep=%.6f baseline=%.6f hash=%016llX "
            "m=[%.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f]",
            presetName, index, type,
            static_cast<unsigned long long>(event.jobSequence),
            static_cast<unsigned long long>(event.present), event.camera,
            event.output, event.threadId, event.separation, event.baseline,
            static_cast<unsigned long long>(HashSepProbeMatrix(event.matrix)),
            event.matrix[0], event.matrix[1], event.matrix[2], event.matrix[3],
            event.matrix[4], event.matrix[5], event.matrix[6], event.matrix[7],
            event.matrix[8], event.matrix[9], event.matrix[10], event.matrix[11],
            event.matrix[12], event.matrix[13], event.matrix[14], event.matrix[15]);
    }
}

void MaybeDumpViewCbPayload(std::uint64_t present)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (g_viewCbPayloadDumped.load(std::memory_order_acquire))
        return;
    const auto target = g_viewCbPayloadDumpAtPresent.load(std::memory_order_acquire);
    if (!target || present < target)
        return;
    for (const auto& snapshot : g_viewCbPayloads)
    {
        if (!snapshot.generation.load(std::memory_order_acquire))
            return;
    }
    bool expected = false;
    if (!g_viewCbPayloadDumped.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    std::array<std::array<std::uint8_t, kViewCbPayloadBytes>, 3> payloads{};
    std::uint64_t generations[3]{}, presents[3]{}, hashes[3]{};
    for (std::size_t mode = 0; mode < payloads.size(); ++mode)
    {
        auto& snapshot = g_viewCbPayloads[mode];
        AcquireSRWLockShared(&snapshot.lock);
        payloads[mode] = snapshot.bytes;
        generations[mode] = snapshot.generation.load(std::memory_order_relaxed);
        presents[mode] = snapshot.present;
        hashes[mode] = snapshot.hash;
        ReleaseSRWLockShared(&snapshot.lock);
    }
    Log("[VSCB992_DUMP] resource=%p generation=%llu/%llu/%llu "
        "present=%llu/%llu/%llu hash=%016llX/%016llX/%016llX",
        reinterpret_cast<void*>(g_viewCbPayloadResource.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(generations[0]),
        static_cast<unsigned long long>(generations[1]),
        static_cast<unsigned long long>(generations[2]),
        static_cast<unsigned long long>(presents[0]),
        static_cast<unsigned long long>(presents[1]),
        static_cast<unsigned long long>(presents[2]),
        static_cast<unsigned long long>(hashes[0]),
        static_cast<unsigned long long>(hashes[1]),
        static_cast<unsigned long long>(hashes[2]));
    for (std::size_t offset = 0; offset < kViewCbPayloadBytes; offset += 16)
    {
        std::uint32_t words[3][4]{};
        float values[3][4]{};
        for (std::size_t mode = 0; mode < 3; ++mode)
        {
            std::memcpy(words[mode], payloads[mode].data() + offset, 16);
            std::memcpy(values[mode], words[mode], 16);
        }
        Log("[VSCB992_DATA] off=%03zX "
            "m=%08X/%08X/%08X/%08X(%.6g,%.6g,%.6g,%.6g) "
            "o0=%08X/%08X/%08X/%08X(%.6g,%.6g,%.6g,%.6g) "
            "o1=%08X/%08X/%08X/%08X(%.6g,%.6g,%.6g,%.6g)",
            offset,
            words[0][0], words[0][1], words[0][2], words[0][3],
            values[0][0], values[0][1], values[0][2], values[0][3],
            words[1][0], words[1][1], words[1][2], words[1][3],
            values[1][0], values[1][1], values[1][2], values[1][3],
            words[2][0], words[2][1], words[2][2], words[2][3],
            values[2][0], values[2][1], values[2][2], values[2][3]);
    }

    std::array<std::array<std::uint8_t, kViewCbPayloadBytes>, 3> written{};
    std::uint64_t writeGenerations[3]{}, writePresents[3]{}, writeJobs[3]{},
        writeSources[3]{};
    std::uint32_t writeThreads[3]{}, writeOutputs[3]{};
    for (std::size_t mode = 0; mode < written.size(); ++mode)
    {
        auto& snapshot = g_viewCbPayloadsAtWrite[mode];
        AcquireSRWLockShared(&snapshot.lock);
        written[mode] = snapshot.bytes;
        writeGenerations[mode] = snapshot.generation.load(std::memory_order_relaxed);
        writePresents[mode] = snapshot.present;
        writeJobs[mode] = snapshot.jobSequence;
        writeSources[mode] = snapshot.sourceGeneration;
        writeThreads[mode] = snapshot.threadId;
        writeOutputs[mode] = snapshot.trueOutput;
        ReleaseSRWLockShared(&snapshot.lock);
    }
    Log("[VSCB992W_DUMP] generation=%llu/%llu/%llu present=%llu/%llu/%llu "
        "job=%llu/%llu/%llu source=%llu/%llu/%llu thread=%u/%u/%u output=%u/%u/%u",
        static_cast<unsigned long long>(writeGenerations[0]),
        static_cast<unsigned long long>(writeGenerations[1]),
        static_cast<unsigned long long>(writeGenerations[2]),
        static_cast<unsigned long long>(writePresents[0]),
        static_cast<unsigned long long>(writePresents[1]),
        static_cast<unsigned long long>(writePresents[2]),
        static_cast<unsigned long long>(writeJobs[0]),
        static_cast<unsigned long long>(writeJobs[1]),
        static_cast<unsigned long long>(writeJobs[2]),
        static_cast<unsigned long long>(writeSources[0]),
        static_cast<unsigned long long>(writeSources[1]),
        static_cast<unsigned long long>(writeSources[2]),
        writeThreads[0], writeThreads[1], writeThreads[2],
        writeOutputs[0], writeOutputs[1], writeOutputs[2]);
    for (std::size_t offset = 0; offset < kViewCbPayloadBytes; offset += 16)
    {
        std::uint32_t words[3][4]{};
        float values[3][4]{};
        for (std::size_t mode = 0; mode < 3; ++mode)
        {
            std::memcpy(words[mode], written[mode].data() + offset, 16);
            std::memcpy(values[mode], words[mode], 16);
        }
        Log("[VSCB992W_DATA] off=%03zX "
            "m=%08X/%08X/%08X/%08X(%.6g,%.6g,%.6g,%.6g) "
            "o0=%08X/%08X/%08X/%08X(%.6g,%.6g,%.6g,%.6g) "
            "o1=%08X/%08X/%08X/%08X(%.6g,%.6g,%.6g,%.6g)",
            offset,
            words[0][0], words[0][1], words[0][2], words[0][3],
            values[0][0], values[0][1], values[0][2], values[0][3],
            words[1][0], words[1][1], words[1][2], words[1][3],
            values[1][0], values[1][1], values[1][2], values[1][3],
            words[2][0], words[2][1], words[2][2], words[2][3],
            values[2][0], values[2][1], values[2][2], values[2][3]);
    }

    alignas(16) float native[2][2][16]{};
    std::uint64_t nativeJob = 0, nativePresent = 0;
    std::uint32_t nativeThread = 0, nativeReady = 0;
    float nativeSeparation = 0.0f, nativeBaseline = 0.0f;
    AcquireSRWLockShared(&g_nativeEyePairLatest.lock);
    std::memcpy(native, g_nativeEyePairLatest.matrices, sizeof(native));
    nativeJob = g_nativeEyePairLatest.jobSequence;
    nativePresent = g_nativeEyePairLatest.present;
    nativeThread = g_nativeEyePairLatest.threadId;
    nativeReady = g_nativeEyePairLatest.readyMask;
    nativeSeparation = g_nativeEyePairLatest.separation;
    nativeBaseline = g_nativeEyePairLatest.baseline;
    ReleaseSRWLockShared(&g_nativeEyePairLatest.lock);
    Log("[NATEYE_DUMP] job=%llu present=%llu thread=%u ready=%u "
        "separation=%.6f baseline=%.6f",
        static_cast<unsigned long long>(nativeJob),
        static_cast<unsigned long long>(nativePresent),
        nativeThread, nativeReady, nativeSeparation, nativeBaseline);
    for (std::size_t kind = 0; kind < 2; ++kind)
    {
        if (!(nativeReady & (1u << kind)))
            continue;
        for (std::size_t eye = 0; eye < 2; ++eye)
        {
            for (std::size_t row = 0; row < 4; ++row)
            {
                std::uint32_t words[4]{};
                float values[4]{};
                std::memcpy(words, &native[kind][eye][row * 4], 16);
                std::memcpy(values, words, 16);
                Log("[NATEYE_DATA] kind=%s eye=%zu row=%zu "
                    "%08X/%08X/%08X/%08X(%.6g,%.6g,%.6g,%.6g)",
                    kind == 0 ? "proj" : "view", eye, row,
                    words[0], words[1], words[2], words[3],
                    values[0], values[1], values[2], values[3]);
            }
        }
    }
}

// [CAMPARAM] Confirm this executable is the one the table address was derived
// from before reading or writing engine globals.
bool ValidateCameraParamTable()
{
    if (g_camParamValidated.load(std::memory_order_acquire))
        return true;
    if (!g_exeBase)
        return false;
    const auto execute = g_exeBase + kCamParamExecuteRva;
    if (!IsReadable(reinterpret_cast<const void*>(execute),
            sizeof(kCamParamSignature)))
        return false;
    bool match = true;
    __try
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(execute);
        for (std::size_t index = 0; index < sizeof(kCamParamSignature); ++index)
        {
            if (bytes[index] != kCamParamSignature[index])
            {
                match = false;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        match = false;
    }
    if (!match)
    {
        static std::atomic_bool logged{};
        bool expected = false;
        if (logged.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
            Log("[CAMPARAM] signature MISMATCH at +0x%llX - table lane disabled",
                static_cast<unsigned long long>(kCamParamExecuteRva));
        return false;
    }
    g_camParamValidated.store(true, std::memory_order_release);
    Log("[CAMPARAM] validated execute=+0x%llX table=+0x%llX guard=+0x%llX "
        "count=%u",
        static_cast<unsigned long long>(kCamParamExecuteRva),
        static_cast<unsigned long long>(kCamParamTableRva),
        static_cast<unsigned long long>(kCamParamGuardRva), kCamParamCount);
    return true;
}

std::uint32_t* CameraParamTable()
{
    if (!ValidateCameraParamTable())
        return nullptr;
    const auto guard = reinterpret_cast<const std::uint64_t*>(
        g_exeBase + kCamParamGuardRva);
    if (!IsReadable(guard, sizeof(std::uint64_t)))
        return nullptr;
    bool ready = false;
    __try
    {
        ready = *guard != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    if (!ready)
        return nullptr;
    auto* table = reinterpret_cast<std::uint32_t*>(
        g_exeBase + kCamParamTableRva);
    if (!IsWritable(table, kCamParamCount * sizeof(std::uint32_t)))
        return nullptr;
    return table;
}

float CamParamAsFloat(std::uint32_t raw)
{
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::uint32_t CamParamFromFloat(float value)
{
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return raw;
}

// [CAMPARAM] Diff the table every frame.  The slot the game moves to the
// installed asset's degree value is BCPT_FOV; slots that move when the user
// enters first person or a cutscene answer whether those modes override the
// same parameter or a different one.
void PollCameraParamTable()
{
    auto* table = CameraParamTable();
    if (!table)
        return;
    const float reference =
        g_camParamReferenceDegrees.load(std::memory_order_relaxed);
    __try
    {
        if (!g_camParamSnapshotTaken.load(std::memory_order_acquire))
        {
            std::memcpy(g_camParamSnapshot.data(), table,
                kCamParamCount * sizeof(std::uint32_t));
            g_camParamBaseline = g_camParamSnapshot;
            g_camParamSnapshotTaken.store(true, std::memory_order_release);
            Log("[CAMPARAM] baseline captured, reference=%.1f degrees",
                reference);
            return;
        }

        // Count first so a whole-table repopulation is absorbed as a new
        // baseline instead of flooding the change log.
        std::uint32_t changed = 0;
        for (std::uint32_t index = 0; index < kCamParamCount; ++index)
        {
            if (table[index] == g_camParamSnapshot[index])
                continue;
            if (table[index] != g_camParamOurWrite[index])
                ++changed;
        }
        if (changed == 0)
            return;

        const bool bulk = changed >= kCamParamBulkChangeThreshold;
        std::uint32_t fovCandidates = 0;
        int fovUnique = -1;
        for (std::uint32_t index = 0; index < kCamParamCount; ++index)
        {
            const auto value = table[index];
            if (value == g_camParamSnapshot[index])
                continue;
            const bool ours = value == g_camParamOurWrite[index];
            g_camParamSnapshot[index] = value;
            if (ours)
                continue;
            // Engine-authored value: this is the truth all scaling derives
            // from, so record it before any of this mod's writes land.
            g_camParamBaseline[index] = value;
            const float degrees = CamParamAsFloat(value);
            if (degrees == reference)
            {
                ++fovCandidates;
                fovUnique = static_cast<int>(index);
            }
            if (bulk)
            {
                // Only the FOV-plausible slots of a repopulation are worth
                // naming; the rest are distances, offsets and rates.
                if (degrees >= kCamParamFovLow && degrees <= kCamParamFovHigh &&
                    g_camParamCandidateLogs.fetch_add(1,
                        std::memory_order_relaxed) < 40)
                    Log("[CAMPARAM] bulk slot=%u = %.3f degrees", index,
                        degrees);
                continue;
            }
            if (g_camParamChangeLogs.fetch_add(1, std::memory_order_relaxed) < 600)
                Log("[CAMPARAM] slot=%u changed -> %.4f (0x%08X) present=%llu",
                    index, degrees, value,
                    static_cast<unsigned long long>(
                        g_presentCount.load(std::memory_order_relaxed)));
        }
        if (bulk)
        {
            const auto fills =
                g_camParamBulkFills.fetch_add(1, std::memory_order_relaxed) + 1;
            Log("[CAMPARAM] bulk fill #%llu absorbed (%u slots) - baseline reset",
                static_cast<unsigned long long>(fills), changed);
        }
        if (fovCandidates == 1 &&
            g_camParamFovIndex.load(std::memory_order_relaxed) < 0)
        {
            g_camParamFovIndex.store(fovUnique, std::memory_order_release);
            Log("[CAMPARAM] FOV slot=%d identified by engine write of %.1f",
                fovUnique, reference);
        }
        else if (fovCandidates > 1)
            Log("[CAMPARAM] %u slots took the reference value - set "
                "FovParamIndex in ffxv-vr.ini", fovCandidates);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// [CAMPARAM] Drive the identified slot from the Insert slider.  Re-asserted
// every frame so a camera mode that rewrites the parameter cannot win.
void DriveCameraParamFov()
{
    // Drive whenever AER is on, without requiring the override toggle: the
    // asset writes slot 7 once at boot and a later preset overwrites it, so
    // the wide value must be re-asserted.  The checkbox still works for
    // flat-screen use.
    if (!g_liveFovOverrideEnabled.load(std::memory_order_acquire) &&
        !VrCameraEnabled())
        return;
    const auto index = g_camParamFovIndex.load(std::memory_order_acquire);
    if (index < 0 || static_cast<std::uint32_t>(index) >= kCamParamCount)
        return;
    if (!g_camParamSnapshotTaken.load(std::memory_order_acquire))
        return;
    auto* table = CameraParamTable();
    if (!table)
        return;
    const float degrees = g_liveFovDegrees.load(std::memory_order_acquire);
    if (!(degrees > 10.0f) || !(degrees < 179.0f))
        return;
    // Kick: after F9 the setter hook is armed but nothing calls the setter
    // until an FOV event, so the headset would stay at the old lens until the
    // slider moved.  Until the first [FOVSET] hit lands, alternate
    // the slot-7 value by 1/16 degree (invisible) so the engine's on-change
    // consumer re-fires the setter, which the hook then overrides.
    float kicked = degrees;
    if (g_fovSetArmed.load(std::memory_order_acquire) &&
        g_fovSetHits.load(std::memory_order_relaxed) == 0 &&
        ((g_presentCount.load(std::memory_order_relaxed) >> 1) & 1))
        kicked += 0.0625f;
    // The table stores float bit patterns; writing an integer here would put
    // a denormal in the slot, which is what a raw integer 100 would have been.
    const auto target = CamParamFromFloat(kicked);
    const float reference =
        g_camParamReferenceDegrees.load(std::memory_order_relaxed);
    const bool allModes = g_camParamAllModes.load(std::memory_order_acquire);
    const float ratio = reference > 1.0f ? degrees / reference : 1.0f;
    __try
    {
        if (table[index] != target)
        {
            table[index] = target;
            g_camParamOurWrite[index] = target;
            g_camParamSnapshot[index] = target;
            g_camParamLastWritten.store(target, std::memory_order_relaxed);
            const auto count =
                g_camParamDrives.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 4 || (count % 600) == 0)
                Log("[CAMPARAM] drive slot=%d <- %.1f degrees count=%llu",
                    index, degrees, static_cast<unsigned long long>(count));
        }
        // First person and cutscenes do not read the other FOV-plausible
        // table slots (scaling them changes nothing), so the all-modes
        // checkbox drives the camera-object [FOVSINK] instead.
        (void)allModes;
        (void)ratio;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// [SPLITGEO], once every ~5 s in native stereo.
//
// Both matrices are world-to-view (row 3 = -(eye . axis)).  The left eye is
// the committed head matrix; the right eye is the same rotation with the
// right-eye seat's row 3.  Each is inverted to a real world eye position with
// ViewEyePosition, and their difference is expressed in the camera's own
// axes.  A correct rig reads (separation, 0, 0) at EVERY camera heading; any
// up/forward component, or an x that changes with heading, is a doubled image.
// Unlike the checks before it, this cannot agree with itself by construction:
// it compares two independently offset matrices through the full inverse.
void LogSplitGeometry()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!g_stereoEnabled.load(std::memory_order_acquire))
        return;
    static std::uint64_t lastPresent = 0;
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    if (present - lastPresent < 300)
        return;
    lastPresent = present;
    float axes[9]{};
    for (int cell = 0; cell < 9; ++cell)
        axes[cell] = g_splitLeftAxes[cell].load(std::memory_order_relaxed);
    // Rebuild both matrices from the shared rotation (columns) and each row 3.
    const auto build = [&](float* out, const std::atomic<float>* row3)
    {
        std::memset(out, 0, sizeof(float) * 16);
        out[0] = axes[0]; out[4] = axes[1]; out[8] = axes[2];
        out[1] = axes[3]; out[5] = axes[4]; out[9] = axes[5];
        out[2] = axes[6]; out[6] = axes[7]; out[10] = axes[8];
        out[12] = row3[0].load(std::memory_order_relaxed);
        out[13] = row3[1].load(std::memory_order_relaxed);
        out[14] = row3[2].load(std::memory_order_relaxed);
        out[15] = 1.0f;
    };
    float left[16]{};
    float right[16]{};
    build(left, g_splitLeftEye);
    build(right, g_splitRightFinal);
    float eyeLeft[3]{};
    float eyeRight[3]{};
    ViewEyePosition(left, eyeLeft);
    ViewEyePosition(right, eyeRight);
    const float d[3] = {eyeRight[0] - eyeLeft[0], eyeRight[1] - eyeLeft[1],
        eyeRight[2] - eyeLeft[2]};
    const auto dot = [&](int column)
    {
        return d[0] * axes[column * 3] + d[1] * axes[column * 3 + 1] +
            d[2] * axes[column * 3 + 2];
    };
    float baseline = 0.0f;
    float scale = 0.0f;
    __try
    {
        baseline = g_stereoBaselineAddress ? *g_stereoBaselineAddress : 0.0f;
        scale = g_stereoScaleAddress ? *g_stereoScaleAddress : 0.0f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    const float heading = std::atan2(axes[2], axes[0]) * 180.0f / 3.14159265f;
    Log("[SPLITGEO] expected=(%.3f,0,0) m  measured right/up/back=(%+.3f,%+.3f,%+.3f) m  "
        "cameraHeading=%.0fdeg  rightFromLeft=%s  engineSkew=%.7f symmetric=%d samples L/R=%llu/%llu",
        baseline * 0.01f, dot(0), dot(1), dot(2), heading,
        std::fabs(g_splitRightSource[0].load(std::memory_order_relaxed) -
            g_splitLeftEye[0].load(std::memory_order_relaxed)) < 0.01f ? "yes" : "NO",
        scale > 1e-3f ? baseline / scale : 0.0f,
        g_nativeSymmetric.load(std::memory_order_relaxed) ? 1 : 0,
        static_cast<unsigned long long>(g_splitLeftSamples.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_splitRightSamples.load(std::memory_order_relaxed)));
}

std::uint8_t ReadStereoGateByte()
{
    if (!g_stereoEnableAddress)
        return 0xFF;
    __try
    {
        return *g_stereoEnableAddress;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0xFE;
    }
}

void WriteStereoGateByte(std::uint8_t value)
{
    if (!g_stereoEnableAddress)
        return;
    __try
    {
        *g_stereoEnableAddress = value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void SetAer(bool enable);
void SetStereo(bool enable);

// Presenter side of the mono fallback, lagged two presents behind the gate
// write in both directions: the engine's next frame is the first rendered
// with the new output count, and switching the presentation on the same
// present showed a half-frame for a frame.
// [VIDEOFIT] presents since the engine last built a stereo eye pair.  In
// stereo 3D it builds exactly two per present (view + projection); videos
// and loading screens build none.
std::uint32_t EyeBuildQuietPresents()
{
    static std::uint64_t lastBuilt = 0;
    static std::uint32_t quiet = 0;
    const auto built = g_nativeEyePairsBuilt.load(std::memory_order_relaxed);
    if (built != lastBuilt) { lastBuilt = built; quiet = 0; }
    else if (quiet < 1000000) ++quiet;
    return quiet;
}

bool PresenterMonoFallback()
{
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    if (g_stereoSuspended.load(std::memory_order_acquire))
        return present - g_autoMonoSuspendPresent.load(std::memory_order_relaxed) >= 2;
    const auto restored = g_autoMonoRestorePresent.load(std::memory_order_relaxed);
    return restored != 0 && present - restored < 2;
}

// [ISEQ] + [AUTOMONO], on the present thread every frame.
void AutoMonoTick()
{
    const auto present = g_presentCount.load(std::memory_order_relaxed);
    const auto lastPoll = g_iseqLastPollPresent.load(std::memory_order_relaxed);
    const bool polling = lastPoll != 0 && present - lastPoll <= 2;

    static bool wasPolling = false;
    static std::uint64_t pollStartPresent = 0;
    static std::uint64_t pollsAtStart = 0;
    static std::uint64_t lastDiffLog = 0;
    static bool havePrevious = false;
    static std::uint8_t previous[kIseqSnapshotBytes];
    if (polling != wasPolling)
    {
        const auto polls = g_iseqPolls.load(std::memory_order_relaxed);
        if (polling)
        {
            pollStartPresent = present;
            pollsAtStart = polls;
        }
        else if (present - pollStartPresent >= 20)
            // Ambient bursts last 3-7 presents; only the long runs are worth
            // a line.
            Log("[ISEQ] interaction action ran %llu presents (%llu polls) self=%llX "
                "stereo=%d conversationActive=%d",
                static_cast<unsigned long long>(present - pollStartPresent),
                static_cast<unsigned long long>(polls - pollsAtStart),
                static_cast<unsigned long long>(g_iseqLastSelf.load(std::memory_order_relaxed)),
                g_stereoEnabled.load(std::memory_order_relaxed) ? 1 : 0,
                g_conversationActive.load(std::memory_order_relaxed));
        wasPolling = polling;
        havePrevious = false;
        lastDiffLog = present;
    }
    if (polling && present - lastDiffLog >= 60)
    {
        lastDiffLog = present;
        std::uint8_t current[kIseqSnapshotBytes];
        std::memcpy(current, g_iseqSnapshot, sizeof(current));
        char line[400];
        int written = 0;
        std::uint32_t changed = 0;
        if (havePrevious)
        {
            for (std::size_t offset = 0; offset + 4 <= kIseqSnapshotBytes; offset += 4)
            {
                std::uint32_t before = 0;
                std::uint32_t after = 0;
                std::memcpy(&before, previous + offset, 4);
                std::memcpy(&after, current + offset, 4);
                if (before == after)
                    continue;
                ++changed;
                if (changed <= 8 && written >= 0 &&
                    written < static_cast<int>(sizeof(line)) - 40)
                    written += std::snprintf(line + written,
                        sizeof(line) - static_cast<std::size_t>(written),
                        " +0x%zX:%08X->%08X", offset, before, after);
            }
        }
        std::memcpy(previous, current, sizeof(previous));
        havePrevious = true;
        Log("[ISEQ] polling %llu presents so far, polls=%llu changedDwords=%u |%s",
            static_cast<unsigned long long>(present - pollStartPresent),
            static_cast<unsigned long long>(g_iseqPolls.load(std::memory_order_relaxed)),
            changed, written > 0 ? line : " (first sample)");
    }

    // [SCRIPTCAM] scripted camera = FOV setter on a non-zero channel, fresh.
    const auto channel = g_fovLastChannel.load(std::memory_order_relaxed);
    const auto lastFovHit = g_fovLastHitPresent.load(std::memory_order_relaxed);
    const bool fovFresh = lastFovHit != 0 && present - lastFovHit <= 120;
    const bool scripted = fovFresh && channel != 0;
    static bool wasScripted = false;
    static std::uint64_t scriptedSince = 0;
    static std::uint32_t loggedChannel = 0xFFFFFFFFu;
    if (fovFresh && channel != loggedChannel)
    {
        Log("[SCRIPTCAM] camera channel %u -> %u mgr=%llX present=%llu stereo=%d active=%d",
            loggedChannel == 0xFFFFFFFFu ? 0u : loggedChannel, channel,
            static_cast<unsigned long long>(g_fovSetManager.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(present),
            g_stereoEnabled.load(std::memory_order_relaxed) ? 1 : 0,
            g_conversationActive.load(std::memory_order_relaxed));
        loggedChannel = channel;
    }
    if (scripted != wasScripted)
    {
        wasScripted = scripted;
        scriptedSince = present;
    }
    static std::uint64_t lastHeartbeat = 0;
    if (present - lastHeartbeat >= 300)
    {
        lastHeartbeat = present;
        Log("[ISEQ] heartbeat polls=%llu channel=%u fovHits zero/nonzero=%llu/%llu fresh=%d "
            "active=%d mode=%d gate=%u aerGateForced=%llu",
            static_cast<unsigned long long>(g_iseqPolls.load(std::memory_order_relaxed)),
            channel,
            static_cast<unsigned long long>(g_fovZeroHits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_fovNonZeroHits.load(std::memory_order_relaxed)),
            fovFresh ? 1 : 0, g_conversationActive.load(std::memory_order_relaxed),
            g_conversationMode.load(std::memory_order_relaxed),
            ReadStereoGateByte(),
            static_cast<unsigned long long>(g_aerGateForced.load(std::memory_order_relaxed)));
    }

    const int active = g_conversationActive.load(std::memory_order_acquire);
    if (active)
    {
        // [AERGATE] AER needs the game in single-output (mono) mode.  The
        // summit cutscene re-asserts two outputs on its own, and this engine's
        // output 0 renders incomplete (CAMDEFER), so half the AER captures
        // came from a broken wireframe output - the wireframe left eye.  Hold
        // the gate at zero every present while AER drives the conversation.
        if (active == 2 && ReadStereoGateByte() == 1)
        {
            WriteStereoGateByte(0);
            const auto forced = g_aerGateForced.fetch_add(1, std::memory_order_relaxed) + 1;
            if (forced <= 3 || (forced % 300) == 0)
                Log("[AERGATE] cutscene re-enabled two outputs; forced back to mono "
                    "(count=%llu present=%llu)",
                    static_cast<unsigned long long>(forced),
                    static_cast<unsigned long long>(present));
        }
        if (!polling && present - lastPoll > kConversationEndGap && !scripted)
        {
            if (active == 1)
            {
                if (g_stereoEnabled.load(std::memory_order_acquire))
                    WriteStereoGateByte(1);
                g_stereoSuspended.store(false, std::memory_order_release);
                g_autoMonoRestorePresent.store(present, std::memory_order_relaxed);
            }
            else if (active == 2)
                SetStereo(true);
            g_conversationActive.store(0, std::memory_order_release);
            Log("[AUTOMONO] conversation over -> native stereo restored (%s) after %llu presents",
                active == 1 ? "two outputs" : "from AER",
                static_cast<unsigned long long>(
                    present - g_autoMonoSuspendPresent.load(std::memory_order_relaxed)));
        }
        return;
    }
    const int mode = g_conversationMode.load(std::memory_order_relaxed);
    if (mode == 0 || !g_stereoEnabled.load(std::memory_order_acquire))
        return;
    const bool byPolling = polling && present - pollStartPresent >= kConversationMinPolling;
    const bool byCamera = scripted && present - scriptedSince >= kScriptedCameraMinPresents;
    if (byPolling || byCamera)
    {
        g_autoMonoSuspendPresent.store(present, std::memory_order_relaxed);
        const auto episode = g_autoMonoEpisodes.fetch_add(1, std::memory_order_relaxed) + 1;
        if (mode == 1)
        {
            WriteStereoGateByte(0);
            g_stereoSuspended.store(true, std::memory_order_release);
        }
        g_conversationActive.store(mode, std::memory_order_release);
        Log("[AUTOMONO] scene detected (%s) -> %s episode=%u",
            byCamera ? "scripted camera channel" : "interaction polling",
            mode == 1 ? "single output (mono, tracked)" : "AER", episode);
        if (mode == 2)
            SetAer(true);
    }
}

bool DeveloperKeysEnabled();   // [DEVKEYS] defined below; forward-declared for Tick()

void Tick()
{
    RecordHotThread();
    AutoMonoTick();
    SeqCensusTick(g_presentCount.load(std::memory_order_relaxed));
    MotionGateTick();
    AiGraphTick();
    AiModeTick();
    TrayTick();
    ActTick();
    CompTick();
    ApplyTick();
    ModeSwTick();
    TaskSkipTick();
    StereoTaskFix::Tick(g_presentCount.load(std::memory_order_relaxed));
    LogSplitGeometry();
    ReportViewCensus();
    // [LENSFEED] publish this frame's widest main-band lens, exactly once per
    // present.  Deterministic: no smoothing, no memory, nothing to latch.
    if (g_lensFeedEnabled.load(std::memory_order_relaxed) &&
        g_aerEnabled.load(std::memory_order_relaxed))
    {
        const auto present = g_presentCount.load(std::memory_order_relaxed);
        const auto framePresent =
            g_lensFramePresent.load(std::memory_order_acquire);
        if (framePresent && present - framePresent <= 2)
        {
            // [LENSHIST] the most-uploaded main-band lens this frame is the
            // raster one; ties go to the wider (smaller p11).
            const auto used = std::min(
                g_lensBucketUsed.load(std::memory_order_acquire), kLensBuckets);
            float p11 = 0.0f;
            float p00 = 0.0f;
            std::uint32_t bestHits = 0;
            for (std::uint32_t i = 0; i < used; ++i)
            {
                const auto hits =
                    g_lensBucketHits[i].load(std::memory_order_acquire);
                if (!hits)
                    continue;
                float candidateP11 = 0.0f;
                float candidateP00 = 0.0f;
                auto bits = g_lensBucketP11Bits[i].load(std::memory_order_relaxed);
                std::memcpy(&candidateP11, &bits, sizeof(candidateP11));
                bits = g_lensBucketP00Bits[i].load(std::memory_order_relaxed);
                std::memcpy(&candidateP00, &bits, sizeof(candidateP00));
                if (!(candidateP11 > 0.01f) || !(candidateP00 > 0.01f))
                    continue;
                if (hits > bestHits || (hits == bestHits && candidateP11 < p11))
                {
                    bestHits = hits;
                    p11 = candidateP11;
                    p00 = candidateP00;
                }
            }
            if (p11 > 0.01f && p00 > 0.01f)
            {
                RetailXr::SetGameProjection(p00, p11, 0.0f, kLensBaselineP00,
                    kLensBaselineP11);
                g_lensFeedWrites.fetch_add(1, std::memory_order_relaxed);
                std::uint32_t p11Bits = 0;
                std::uint32_t p00Bits = 0;
                std::memcpy(&p11Bits, &p11, sizeof(p11Bits));
                std::memcpy(&p00Bits, &p00, sizeof(p00Bits));
                g_lensFrameP11Bits.store(p11Bits, std::memory_order_relaxed);
                g_lensFrameP00Bits.store(p00Bits, std::memory_order_relaxed);
            }
            // One histogram line every few seconds.  From ordinary play this
            // shows whether a mode carries two competing lenses (a selection
            // bug) or one lens whose value moves frame to frame (the game).
            auto lastHist = g_lensHistLogPresent.load(std::memory_order_relaxed);
            if (p11 > 0.01f && present - lastHist >= 300 &&
                g_lensHistLogPresent.compare_exchange_strong(lastHist, present,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
            {
                constexpr float kPi = 3.14159265358979323846f;
                char line[256];
                int written = 0;
                for (std::uint32_t i = 0; i < used && written >= 0 &&
                    written < static_cast<int>(sizeof(line)) - 32; ++i)
                {
                    const auto hits =
                        g_lensBucketHits[i].load(std::memory_order_relaxed);
                    if (!hits)
                        continue;
                    float candidateP11 = 0.0f;
                    const auto bits =
                        g_lensBucketP11Bits[i].load(std::memory_order_relaxed);
                    std::memcpy(&candidateP11, &bits, sizeof(candidateP11));
                    if (!(candidateP11 > 0.01f))
                        continue;
                    const float fovV =
                        2.0f * std::atan(1.0f / candidateP11) * 180.0f / kPi;
                    const int added = std::snprintf(line + written,
                        sizeof(line) - static_cast<std::size_t>(written),
                        " %.1fdeg@+0x%X x%u", fovV,
                        g_lensBucketOffset[i].load(std::memory_order_relaxed),
                        hits);
                    if (added < 0)
                        break;
                    written += added;
                }
                Log("[LENSHIST] claim=%.1fdeg hits=%u lenses=%u |%s",
                    2.0f * std::atan(1.0f / p11) * 180.0f / kPi, bestHits,
                    used, written > 0 ? line : " none");
            }
        }
    }
    // [MONO2D] Loading screens, menus and pre-rendered movies have no 3D
    // camera at all: the engine uploads no main-band projection for those
    // frames.  Alternating eyes across them is worse than pointless - there is
    // no disparity to build, and the alternation puts a shimmer on a flat
    // image - so the presenter shows one frame to both eyes instead.  Two
    // consecutive frames without a projection enter mono and one with it
    // leaves, so a single missed upload cannot flip the view.
    {
        static std::uint64_t lastLensSamples = 0;
        static std::uint32_t quietPresents = 0;
        static bool lastFlat = false;
        const auto samples = g_lensSamples.load(std::memory_order_relaxed);
        if (samples != lastLensSamples)
        {
            lastLensSamples = samples;
            quietPresents = 0;
        }
        else if (quietPresents < 1000)
            ++quietPresents;
        const bool flat = quietPresents >= 2;
        if (flat != lastFlat)
        {
            lastFlat = flat;
            Log("[MONO2D] scene is now %s",
                flat ? "FLAT - one image to both eyes"
                     : "3D - alternating eyes");
        }
        RetailXr::SetSceneIsFlat(flat);
    }
    DrawFindTick();
    SceneCensus::OnPassDrawFrame();
    PollCameraParamTable();
    DriveCameraParamFov();
    const auto count = g_presentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_liveFovOverrideEnabled.load(std::memory_order_relaxed))
    {
        auto lastDump = g_ivFovLastDumpPresent.load(std::memory_order_relaxed);
        if (count - lastDump >= 3600 &&
            g_ivFovLastDumpPresent.compare_exchange_strong(lastDump, count,
                std::memory_order_acq_rel, std::memory_order_relaxed))
            IvFovDumpState("periodic");
    }
    // Head tracking uses current-build dynamic camera ownership. The old
    // UPDATEVIEW_A RVA is 0xCC padding in the live Steam executable.
    const bool devKeys = DeveloperKeysEnabled();   // [DEVKEYS] computed once, reused below
    const bool f9Down = (GetAsyncKeyState(
        g_vrEnableKey.load(std::memory_order_relaxed)) & 0x8000) != 0;   // [KEYBIND]
    const bool f9WasDown = g_f9Down.exchange(f9Down,
        std::memory_order_acq_rel);
    if (f9Down && !f9WasDown)
    {
        // [KEYBIND]/[MODETRACK] "enable head tracking", not "force stereo
        // mode": from mono it still starts stereo (the documented
        // get-started flow - unchanged), but if a VR mode is already active
        // this no longer switches you out of it, it just re-arms tracking.
        if (VrCameraEnabled())
            RediscoverDynamicHeadWriter();
        else if (g_preferredVrMode.load(std::memory_order_relaxed) == 3)
            SetMono(true);   // [MONOVR]
        else if (g_preferredVrMode.load(std::memory_order_relaxed) == 2)
            SetAer(true);   // [VRMODESAVE]
        else
            SetStereo(true);
    }
    {
        const bool monoDown = (GetAsyncKeyState(g_monoKey.load(std::memory_order_relaxed)) & 0x8000) != 0;
        if (monoDown && !g_monoKeyDown.exchange(true, std::memory_order_acq_rel))
        {
            Log("[MONOKEY] switch to Mono pressed");
            SetMono(true);
        }
        else if (!monoDown)
            g_monoKeyDown.store(false, std::memory_order_release);
    }

    // [FPSAB] One-key AER toggle (numpad 0) so a native/AER/mono comparison can
    // be run standing in one spot in the headset without opening the menu.
    // Same call the menu checkbox makes; SetAer turns native stereo off itself
    // when it enables AER.  Numpad is used by neither the game nor Steam.
    const bool numpad0Down = (GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0;
    const bool numpad0WasDown = g_numpad0Down.exchange(numpad0Down,
        std::memory_order_acq_rel);
    if (devKeys && numpad0Down && !numpad0WasDown)
    {
        const bool enable = !g_aerEnabled.load(std::memory_order_acquire);
        SetAer(enable);
        Log("[FPSAB] numpad0 -> AER %s", enable ? "ON" : "off");
        Beep(enable ? 1200 : 600, 120);
    }
    // [EYEFULL] numpad 3: one-shot capture of every full-size colour target,
    // to identify the engine's full-resolution per-eye images.
    const bool numpad3Down = (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) != 0;
    const bool numpad3WasDown = g_numpad3Down.exchange(numpad3Down, std::memory_order_acq_rel);
    if (devKeys && numpad3Down && !numpad3WasDown)
    {
        GBufferCensus::ArmColorDump();
        Beep(2000, 90);
        Beep(1400, 90);
    }
    // [AFWFLIP] numpad 2 flips which side the synthesized eye sits on.  One
    // position gives correct depth, the other visibly wrong depth; this is the
    // measurement that settles the engine's eye-label convention.
    const bool numpad2Down = (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) != 0;
    const bool numpad2WasDown = g_numpad2Down.exchange(numpad2Down, std::memory_order_acq_rel);
    if (devKeys && numpad2Down && !numpad2WasDown)
    {
        const bool flip = !g_afwFlip.load(std::memory_order_acquire);
        AerControl::SetAfwFlip(flip);
        Beep(flip ? 1800 : 900, 120);
    }
    // [AFW] numpad 1 toggles the other-eye synthesis for an in-headset A/B.
    const bool numpad1Down = (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;
    const bool numpad1WasDown = g_numpad1Down.exchange(numpad1Down, std::memory_order_acq_rel);
    if (devKeys && numpad1Down && !numpad1WasDown)
    {
        const bool enable = !g_afwEnabled.load(std::memory_order_acquire);
        AerControl::SetAfwEnabled(enable);
        Beep(enable ? 1500 : 500, 120);
    }

    MaybeDumpViewCbPayload(count);

    const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    const bool f10WasDown = g_f10Down.exchange(f10Down, std::memory_order_acq_rel);
    if (devKeys && f10Down && !f10WasDown)
    {
        if (g_aerEnabled.load(std::memory_order_acquire))
        {
            // Stereo-world-scale presets around a neutral 64 mm virtual IPD.
            // world scale = 0.032 / halfEye.
            constexpr float kAerHalfEyeSteps[] = {
                0.021333f, 0.025600f, 0.032000f, 0.042667f
            };
            const float current = g_aerHalfEyeMeters.load(std::memory_order_acquire);
            std::size_t next = 0;
            for (std::size_t step = 0; step < _countof(kAerHalfEyeSteps); ++step)
            {
                if (std::fabs(current - kAerHalfEyeSteps[step]) < 0.001f)
                {
                    next = (step + 1) % _countof(kAerHalfEyeSteps);
                    break;
                }
            }
            g_aerHalfEyeMeters.store(kAerHalfEyeSteps[next], std::memory_order_release);
            Log("[AER] stereo world scale=%.2fx (virtual IPD=%.1fmm, F10)",
                0.032f / kAerHalfEyeSteps[next],
                kAerHalfEyeSteps[next] * 2000.0f);
            Beep(900 + static_cast<int>(next) * 220, 90);
        }
        else if (!g_stereoEnabled.load(std::memory_order_acquire) ||
            !g_stereoBaselineAddress)
        {
            Log("[SEPPROBE] F10 rejected: enable flat stereo with F9 first");
            Beep(300, 140);
        }
        else
        {
            const int previous = g_sepProbePreset.load(std::memory_order_acquire);
            const int next = (previous + 1) %
                static_cast<int>(kBaselineProbeMultipliers.size());
            const float multiplier =
                kBaselineProbeMultipliers[static_cast<std::size_t>(next)];
            const float value = g_savedStereoBaseline * multiplier;
            __try
            {
                *g_stereoSeparationAddress = g_savedStereoSeparation;
                *g_stereoBaselineAddress = value;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            ResetSepProbeCaptures();
            ResetSepProbeEvents();
            g_sepProbePreset.store(next, std::memory_order_release);
            g_sepProbeDumpAtPresent.store(count + 60, std::memory_order_release);
            // Re-arm the correlated payload dump so every baseline preset
            // change produces a fresh VSCB992/VSCB992W/NATEYE dump to diff.
            g_viewCbPayloadDumped.store(false, std::memory_order_release);
            g_viewCbPayloadDumpAtPresent.store(count + 120,
                std::memory_order_release);
            g_camDumpGeneration.fetch_add(1, std::memory_order_acq_rel);
            Log("[SEPPROBE] armed preset=%s separation=%.6f baseline=%.6f "
                "(stock %.6f x %.1f); dump in 60 presents",
                kSepProbePresetNames[static_cast<std::size_t>(next)],
                g_savedStereoSeparation, value, g_savedStereoBaseline, multiplier);
            Beep(900 + next * 350, 90);
        }
    }
    const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool f8WasDown = g_f8Down.exchange(f8Down, std::memory_order_acq_rel);
    if (devKeys && f8Down && !f8WasDown)
    {
        const auto nextMode =
            (g_o0SkewProbeMode.load(std::memory_order_acquire) + 1) %
                kO0SkewModeCount;
        g_o0SkewProbeMode.store(nextMode, std::memory_order_release);
        float referenceSkew = 0.0f;
        std::uint64_t referencePresent = 0;
        AcquireSRWLockShared(&g_mainCamSkewRef.lock);
        referenceSkew = g_mainCamSkewRef.skew;
        referencePresent = g_mainCamSkewRef.present;
        ReleaseSRWLockShared(&g_mainCamSkewRef.lock);
        Log("[O0SKEW] mode=%s (measured=%llu, applied=%llu, swayed=%llu, "
            "objWrites=%llu, objBlocks=%llu, objSkewWrites=%llu, "
            "objSkewBlocks=%llu, seenL=%llu, seenR=%llu, filtered=%llu, "
            "inverseFailed=%llu, refSkew=%.6f, refPresent=%llu)",
            nextMode == kO0SkewModeSwayCam ? "EYECAM"
                : nextMode == kO0SkewModeProbe ? "PROBE"
                : nextMode == kO0SkewModeSway ? "SWAY992"
                : nextMode == kO0SkewModeSwayObj ? "SWAYOBJ" : "OFF",
            static_cast<unsigned long long>(
                g_o0SkewMeasured.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_o0SkewApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_o0SkewSwayed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_objSwayWrites.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_objSwayBlocks.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_objSkewWrites.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_objSkewBlocks.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_objSkewSeenLeft.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_objSkewSeenRight.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_o0SkewFiltered.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_o0SkewInverseFailed.load(std::memory_order_relaxed)),
            referenceSkew,
            static_cast<unsigned long long>(referencePresent));
        // Fresh correlated dump either way so the o0 payload state is provable.
        g_viewCbPayloadDumped.store(false, std::memory_order_release);
        g_viewCbPayloadDumpAtPresent.store(count + 120,
            std::memory_order_release);
        Beep(nextMode == kO0SkewModeSwayCam ? 1100
            : nextMode == kO0SkewModeProbe ? 1250
            : nextMode == kO0SkewModeSway ? 900
            : nextMode == kO0SkewModeSwayObj ? 700 : 450, 80);
    }
    const int activeSepPreset = g_sepProbePreset.load(std::memory_order_acquire);
    if (g_stereoEnabled.load(std::memory_order_acquire) &&
        activeSepPreset >= 0 &&
        activeSepPreset < static_cast<int>(kBaselineProbeMultipliers.size()) &&
        g_stereoSeparationAddress && g_stereoBaselineAddress)
    {
        __try
        {
            *g_stereoSeparationAddress = g_savedStereoSeparation;
            *g_stereoBaselineAddress = g_savedStereoBaseline *
                kBaselineProbeMultipliers[static_cast<std::size_t>(activeSepPreset)];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    auto dumpAt = g_sepProbeDumpAtPresent.load(std::memory_order_acquire);
    if (dumpAt && count >= dumpAt &&
        g_sepProbeDumpAtPresent.compare_exchange_strong(
            dumpAt, 0, std::memory_order_acq_rel))
        DumpSepProbeMatrices();
    // No F8/POSTSKIP toggle: the eye-1 duplicate skip strips per-output
    // shader bindings and renders right-eye skin WHITE, so the guard is
    // pinned off.
    g_postChainSkipEnabled.store(false, std::memory_order_release);
    // Gate watch is fully automatic: armed by SetStereo(true), re-swept so job
    // threads created after arming are covered, self-disarmed after the window.
    if (g_gateWatchArmed.load(std::memory_order_acquire))
    {
        const auto now = GetTickCount();
        // Stay armed while stereo is on; re-sweep so threads created later
        // (job workers, scene loads) are covered too.
        auto lastSweep = g_gateWatchLastSweepMs.load(std::memory_order_acquire);
        if (now - lastSweep >= 2000 &&
            g_gateWatchLastSweepMs.compare_exchange_strong(lastSweep, now,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            ApplyGateWatchToThreads(true);
            std::uint32_t readers = 0;
            for (auto& slot : g_gateWatchReaders)
            {
                if (slot.load(std::memory_order_relaxed))
                    ++readers;
            }
            Log("[GATEWATCH] armed hits=%llu uniqueReaders=%u",
                static_cast<unsigned long long>(g_gateWatchHits.load(std::memory_order_relaxed)),
                readers);
        }
    }
    static std::atomic_bool f11State{false};
    const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool f11WasDown = f11State.exchange(f11Down, std::memory_order_acq_rel);
    if (devKeys && f11Down && !f11WasDown)
    {
        const bool enabled = !g_nearLodRestoreEnabled.load(std::memory_order_acquire);
        g_nearLodRestoreEnabled.store(enabled, std::memory_order_release);
        Log("[NEARLOD] near-actor finest-tier restore %s (F11)",
            enabled ? "ENABLED" : "DISABLED");
        Beep(enabled ? 2300 : 1100, 80);
    }
    static std::atomic_bool f2State{false};
    const bool f2Down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    const bool f2WasDown = f2State.exchange(f2Down, std::memory_order_acq_rel);
    if (devKeys && f2Down && !f2WasDown)
    {
        const bool enabled = !g_boundedFallbackRuntimeEnabled.load(std::memory_order_acquire);
        g_boundedFallbackRuntimeEnabled.store(enabled, std::memory_order_release);
        Log("[LODBOUND] bounded-fallback forcing %s (F2)", enabled ? "ENABLED" : "DISABLED");
        Beep(enabled ? 2100 : 1000, 80);
    }
    static std::atomic_bool f3State{false};
    const bool f3Down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    const bool f3WasDown = f3State.exchange(f3Down, std::memory_order_acq_rel);
    if (devKeys && f3Down && !f3WasDown)
    {
        const bool enabled = !g_viewFlagsFixEnabled.load(std::memory_order_acquire);
        g_viewFlagsFixEnabled.store(enabled, std::memory_order_release);
        Log("[VFIX] view-flags corrective write %s (F3)", enabled ? "ENABLED" : "DISABLED");
        Beep(enabled ? 1900 : 900, 80);
    }
    if (g_viewFlagsFixEnabled.load(std::memory_order_acquire) &&
        (g_stereoEnabled.load(std::memory_order_acquire) ||
            g_everStereo.load(std::memory_order_acquire)) &&
        g_enginePatchesReady.load(std::memory_order_acquire))
    {
        __try
        {
            const auto globalAddr = g_exeBase + kDeferredCtxGlobalRva;
            std::uintptr_t deferredBase = 0;
            if (IsReadable(reinterpret_cast<void*>(globalAddr), sizeof(std::uintptr_t)))
                deferredBase = *reinterpret_cast<std::uintptr_t*>(globalAddr);
            if (deferredBase)
            {
                auto* flags = reinterpret_cast<std::uint32_t*>(
                    deferredBase + kViewCtxOffset + kViewFlagsOffset);
                if (IsReadable(flags, 4) && (*flags & 5u) != 5u)
                {
                    const auto before = *flags;
                    *flags = before | 5u;
                    const auto n = g_viewFlagsCorrections.fetch_add(1,
                        std::memory_order_relaxed) + 1;
                    if (n <= 4)
                        Log("[VFIX] corrected viewFlags 0x%08X -> 0x%08X (#%llu)",
                            before, before | 5u, static_cast<unsigned long long>(n));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    static std::atomic_bool f4State{false};
    const bool f4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    const bool f4WasDown = f4State.exchange(f4Down, std::memory_order_acq_rel);
    if (devKeys && f4Down && !f4WasDown)
    {
        const bool enabled = !g_qualityKeepEnabled.load(std::memory_order_acquire);
        g_qualityKeepEnabled.store(enabled, std::memory_order_release);
        Log("[QMASK] full-quality-in-stereo %s (F4)", enabled ? "ENABLED" : "DISABLED");
        Beep(enabled ? 1700 : 800, 80);
    }
    // No F5/F6 camera toggles: one behaviour, no keys.
    if (g_probeForceEnabled.load(std::memory_order_acquire) &&
        (g_stereoEnabled.load(std::memory_order_acquire) ||
            g_everStereo.load(std::memory_order_acquire)) &&
        g_enginePatchesReady.load(std::memory_order_acquire))
    {
        __try
        {
            auto* force = reinterpret_cast<std::uint8_t*>(g_exeBase + kProbeForceGlobalRva);
            if (IsReadable(force, 1) && *force == 0)
            {
                *force = 1;
                g_probeForcePulses.fetch_add(1, std::memory_order_relaxed);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    // F6: shared-camera fix toggle.
    // F7: +0x4F5 camera handshake toggle.
    static std::atomic_bool f7State{false};
    // F6: headset fill factor (how much of the claimed frustum the game
    // image is stretched to cover).  The game's own FOV is not writable, so
    // this is the coverage knob: 1.0 geometry-honest, higher = fuller with
    // mild magnification.
    static std::atomic_bool f6State{};
    const bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool f6WasDown = f6State.exchange(f6Down, std::memory_order_acq_rel);
    if (devKeys && f6Down && !f6WasDown)
    {
        constexpr float kFillSteps[] = {1.0f, 1.15f, 1.25f, 1.4f, 1.6f};
        const float current = RetailXr::GetFillFactor();
        std::size_t index = 0;
        for (std::size_t step = 0; step < 5; ++step)
        {
            const float delta = kFillSteps[step] - current;
            if (delta > -0.01f && delta < 0.01f)
            {
                index = (step + 1) % 5;
                break;
            }
        }
        RetailXr::SetFillFactor(kFillSteps[index]);
        Log("[FILL] factor=%.2f (F6)", kFillSteps[index]);
        Beep(700 + static_cast<int>(index) * 200, 80);
    }

    const bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool f7WasDown = f7State.exchange(f7Down, std::memory_order_acq_rel);
    if (devKeys && f7Down && !f7WasDown)
    {
        const bool disabled = !g_camFlagStoreDisabled.load(std::memory_order_acquire);
        g_camFlagStoreDisabled.store(disabled, std::memory_order_release);
        ApplyCamFixSite(kCamFixFlagIndex, disabled);
        Log("[CAMFIX] +0x4F5 handshake store %s (F7)",
            disabled ? "DISABLED" : "ACTIVE (engine default)");
        Beep(disabled ? 800 : 2000, 80);
    }
    SceneCensus::OnPresent(g_stereoEnabled.load(std::memory_order_acquire));
    MaybeLogLeakTelemetry();
    if ((count % 120) == 0)
    {
        LogPoolCensus(g_stereoEnabled.load(std::memory_order_acquire));
        LogLightManagerSample(g_stereoEnabled.load(std::memory_order_acquire),
            (count % 600) == 0);
    }
    if (ResearchDiagnostics::Enabled && (count % 600) == 0)
    {
        {
            char nanLine[512]{};
            StringCchPrintfA(nanLine, _countof(nanLine),
                "[NANSCAN] scanned=%llu/%llu/%llu/%llu nan=%llu/%llu/%llu/%llu writers:",
                static_cast<unsigned long long>(g_scannedPayloads[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_scannedPayloads[1].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_scannedPayloads[2].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_scannedPayloads[3].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_nanPayloads[0].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_nanPayloads[1].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_nanPayloads[2].load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_nanPayloads[3].load(std::memory_order_relaxed)));
            int shown = 0;
            for (const auto& cell : g_nanWriters)
            {
                const auto rva = cell.rva.load(std::memory_order_acquire);
                if (!rva || shown >= 6)
                    continue;
                char part[64]{};
                StringCchPrintfA(part, _countof(part), " +0x%llX=%llu",
                    static_cast<unsigned long long>(rva),
                    static_cast<unsigned long long>(cell.count.load(std::memory_order_relaxed)));
                StringCchCatA(nanLine, _countof(nanLine), part);
                ++shown;
            }
            WriteLogLine(nanLine);
        }
        // Re-scan while it is still finding new tables, and for a while
        // after, so a subsystem that starts late is not invisible forever.
        if (g_engineD3dScans.load(std::memory_order_relaxed) < 40)
            TryHookEngineD3dTable();
        // Keep reading the patched slots back for the whole session; a revert
        // that only happens after a load screen would otherwise look exactly
        // like a game that never draws.
        VerifyEngineD3dTables();
        LogUiCensus();
        if (g_gateWatchArmed.load(std::memory_order_acquire))
        {
            std::uint32_t readers = 0;
            for (auto& slot : g_gateWatchReaders)
            {
                if (slot.load(std::memory_order_relaxed))
                    ++readers;
            }
            Log("[GATEWATCH] hits=%llu uniqueReaders=%u",
                static_cast<unsigned long long>(g_gateWatchHits.load(std::memory_order_relaxed)),
                readers);
        }
        Log("[UPDISP] calls(mono/stereo)=%llu/%llu arg0=%p arg1=%p",
            static_cast<unsigned long long>(g_updateDispatchCalls[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_updateDispatchCalls[1].load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(g_updateDispatchArg0.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(g_updateDispatchArg1.load(std::memory_order_relaxed)));
        Log("[CAMWRAP] on=%d calls=%llu restored=%llu",
            g_camWrapEnabled.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long long>(g_camWrapCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_camWrapRestored.load(std::memory_order_relaxed)));
        Log("[VRHEAD] phase=%u source=%p writer=+0x%llX calls=%llu applied=%llu rejected=%llu",
            static_cast<unsigned>(g_dynamicHeadPhase.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(g_dynamicHeadSource.load(std::memory_order_relaxed)),
            g_dynamicHeadWriter.load(std::memory_order_relaxed) >= g_exeBase
                ? static_cast<unsigned long long>(
                    g_dynamicHeadWriter.load(std::memory_order_relaxed) - g_exeBase) : 0ull,
            static_cast<unsigned long long>(
                g_headTrackCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_headTrackApplied.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_headTrackRejected.load(std::memory_order_relaxed)));
        Log("[EYEPAIR] built=%llu committed=%llu fallback=%llu",
            static_cast<unsigned long long>(
                g_nativeEyePairsBuilt.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_symmetricEyeCommits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                g_symmetricEyeFallbacks.load(std::memory_order_relaxed)));
        Log("[NEARLOD] on=%d nearActorsSeen=%llu forcedToTier0=%llu",
            g_nearLodRestoreEnabled.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long long>(g_nearLodSeen.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_nearLodForced.load(std::memory_order_relaxed)));
        Log("[QMASK] keepOn=%d substituted=%llu passthrough=%llu",
            g_qualityKeepEnabled.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long long>(g_qualityKeepHits.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_qualityPassthrough.load(std::memory_order_relaxed)));
        Log("[PROBEW] calls clean/stereo/post=%llu/%llu/%llu forceOn=%d pulses=%llu",
            static_cast<unsigned long long>(g_probewCalls[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_probewCalls[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_probewCalls[2].load(std::memory_order_relaxed)),
            g_probeForceEnabled.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long long>(g_probeForcePulses.load(std::memory_order_relaxed)));
        Log("[OUTSET] clamp=%d phaseTid=%lu restores 0/1/other=%llu/%llu/%llu healed=%llu clamped=%llu",
            g_stereoFiberClampEnabled.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long>(g_phaseThreadId.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(g_outsetValue0.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_outsetValue1.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_outsetOther.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_outsetHealed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_outsetClamped.load(std::memory_order_relaxed)));
        Log("[ROTGATE] gateTrue=%d hits=%llu | rotator eye0=%llu guarded=%llu failed=%llu",
            g_bankRotGateTrue.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long long>(g_policyTrueBankRotGate.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_bankRotEye0.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_bankRotGuarded.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_bankRotGuardFailed.load(std::memory_order_relaxed)));
        Log("[POSTSKIP] enabled=%d exec/skip 0=%llu/%llu 1=%llu/%llu 2=%llu/%llu "
            "3=%llu/%llu 4=%llu/%llu 5=%llu/%llu",
            g_postChainSkipEnabled.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long long>(g_postChainExecuted[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainSkipped[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainExecuted[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainSkipped[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainExecuted[2].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainSkipped[2].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainExecuted[3].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainSkipped[3].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainExecuted[4].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainSkipped[4].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainExecuted[5].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_postChainSkipped[5].load(std::memory_order_relaxed)));
    }
    if ((count % 600) == 0)
    {
        Log("heartbeat present=%llu stereo=%d policy(camera=%llu presenter=%llu lifecycle=%llu/%llu alias=%llu) cache(pass=%llu skip=%llu construct=%llu state=%u) upmtx(real=%llu/%llu inject=%llu fail=%llu)",
            static_cast<unsigned long long>(count),
            g_stereoEnabled.load(std::memory_order_acquire) ? 1 : 0,
            static_cast<unsigned long long>(g_policyTrueCamera.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_policyTruePresenter.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_policyTrueLifecycle0.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_policyTrueLifecycle1.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_policyAliased.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_matrixCachePassed.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_matrixCacheSkipped.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_matrixCacheConstructed.load(std::memory_order_relaxed)),
            g_matrixCacheConstructionState.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(g_uploadMatricesOutput0.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_uploadMatricesOutput1.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_uploadMatricesInjected.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_uploadMatricesInjectionFailed.load(std::memory_order_relaxed)));
    }
}

// [NOHEADSET] Desktop test mode (ini [AER] NoHeadset=1).  Native stereo and
// every engine probe work without a VR session - the conversation hang is
// inside the engine's two-output path - but with no headset the presenter
// retries a synchronous OpenXR bootstrap on the render thread every 5 s.
// This skips the presenter entirely so the game runs plainly on the monitor.
// [DEVKEYS] Off by default; ini [Debug] DeveloperKeys=1 re-arms
// every research hotkey without a rebuild.  Same lazy-read-once shape
// as NoHeadsetMode() just above.
bool DeveloperKeysEnabled()
{
    static const bool value = [] {
        wchar_t path[MAX_PATH]{};
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&DeveloperKeysEnabled), &self) ||
            !GetModuleFileNameW(self, path, MAX_PATH))
            return false;
        wchar_t* slash = std::wcsrchr(path, L'\\');
        if (!slash)
            return false;
        std::wcsncpy(slash + 1, L"ffxv-vr.ini", MAX_PATH - (slash + 1 - path));
        path[MAX_PATH - 1] = L'\0';
        const bool on = GetPrivateProfileIntW(L"Debug", L"DeveloperKeys", 0, path) != 0;
        Log("[DEVKEYS] research/debug hotkeys %s (ini [Debug] DeveloperKeys=%d)",
            on ? "ENABLED" : "off - release build", on ? 1 : 0);
        return on;
    }();
    return value;
}

bool NoHeadsetMode()
{
    static const bool value = [] {
        wchar_t path[MAX_PATH]{};
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&NoHeadsetMode), &self) ||
            !GetModuleFileNameW(self, path, MAX_PATH))
            return false;
        wchar_t* slash = std::wcsrchr(path, L'\\');
        if (!slash)
            return false;
        std::wcsncpy(slash + 1, L"ffxv-vr.ini", MAX_PATH - (slash + 1 - path));
        path[MAX_PATH - 1] = L'\0';
        const bool on = GetPrivateProfileIntW(L"AER", L"NoHeadset", 0, path) != 0;
        Log("[NOHEADSET] desktop test mode %s (ini [AER] NoHeadset=%d)", on ? "ON - OpenXR presenter skipped" : "off",
            on ? 1 : 0);
        return on;
    }();
    return value;
}

// [FRAMETIME] Where does a present interval go?  Four buckets per frame:
//   engine  = previous hook exit -> this hook entry (the game's own work)
//   mod     = hook entry -> real Present call, minus xrWaitFrame (capture,
//             copies, HUD, submit)
//   xrWait  = blocked inside xrWaitFrame (the runtime's own pacing)
//   present = inside the real DXGI Present (swapchain/vsync/GPU back-pressure)
// Reported as p50/p90 over 300 presents.  Decides whether a 1:1 lock at a
// given refresh is reachable and what to trim if not.
struct FrameTimingSample { double engine, mod, xrWait, present; };
std::array<FrameTimingSample, 300> g_frameTimingRing{};
std::size_t g_frameTimingCount{};
long long g_frameTimingLastExitQpc{};

long long FrameQpc()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

double QpcToMs(long long ticks)
{
    static const double frequency = []
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? static_cast<double>(value.QuadPart) : 1.0;
    }();
    return static_cast<double>(ticks) * 1000.0 / frequency;
}

void NoteFrameTiming(long long entryQpc, long long beforePresentQpc, long long afterPresentQpc)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const double xrWaitMs = static_cast<double>(RetailXr::GetLastXrWaitMicros()) / 1000.0;
    FrameTimingSample sample{};
    sample.engine = g_frameTimingLastExitQpc ? QpcToMs(entryQpc - g_frameTimingLastExitQpc) : 0.0;
    sample.mod = std::max(0.0, QpcToMs(beforePresentQpc - entryQpc) - xrWaitMs);
    sample.xrWait = xrWaitMs;
    sample.present = QpcToMs(afterPresentQpc - beforePresentQpc);
    g_frameTimingLastExitQpc = afterPresentQpc;
    g_frameTimingRing[g_frameTimingCount++] = sample;
    if (g_frameTimingCount < g_frameTimingRing.size())
        return;
    g_frameTimingCount = 0;
    auto percentile = [](double FrameTimingSample::*field, double fraction)
    {
        std::array<double, 300> values{};
        for (std::size_t index = 0; index < values.size(); ++index)
            values[index] = g_frameTimingRing[index].*field;
        const auto rank = static_cast<std::size_t>(fraction * (values.size() - 1));
        std::nth_element(values.begin(), values.begin() + rank, values.end());
        return values[rank];
    };
    Log("[FRAMETIME] ms p50/p90: engine=%.2f/%.2f mod=%.2f/%.2f xrWait=%.2f/%.2f present=%.2f/%.2f "
        "(engine+mod = frame work; a 1:1 lock needs p90 work under one display period)",
        percentile(&FrameTimingSample::engine, 0.5), percentile(&FrameTimingSample::engine, 0.9),
        percentile(&FrameTimingSample::mod, 0.5), percentile(&FrameTimingSample::mod, 0.9),
        percentile(&FrameTimingSample::xrWait, 0.5), percentile(&FrameTimingSample::xrWait, 0.9),
        percentile(&FrameTimingSample::present, 0.5), percentile(&FrameTimingSample::present, 0.9));
}

HRESULT HookPresent(PresentFn g_realPresent, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    const UINT originalFlags=flags;
    if(!NoHeadsetMode()) flags=DesktopPresentPolicy::PresentFlags(swapChain,syncInterval,flags);
    PresentTiming::Frame timingFrame(swapChain,syncInterval,flags,g_stereoEnabled.load()?1u:(g_aerEnabled.load()?2u:0u));
    const long long frameEntryQpc = FrameQpc();
    PresentProfiler::NotePresentThread(GetCurrentThreadId());
    PerfCensus::presentTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    RetailXr::BeginHeadPosePresent();
    Tick();
    WatchContextVtables(swapChain);   // [VTWATCH]
    GBufferCensus::OnPresent(swapChain, g_aerEnabled.load(std::memory_order_acquire));
    DlssProbe::OnPresent(g_aerEnabled.load(std::memory_order_acquire),
        g_stereoEnabled.load(std::memory_order_relaxed), AerControl::GetRenderEye());
    DlssProbe::OnPresentGpu(swapChain);
    DlssUpscaler::OnPresent();
    UiHook::SetNativeStereo(g_stereoEnabled.load(std::memory_order_relaxed));
    UiHook::SetWorldIcons(VrCameraEnabled());   // [AERICONS]
    UiHook::OnPresent(swapChain);
    {
        // [HUDLAYER] Hand this frame's interface image to the presenter and
        // learn whether a headset is consuming it; the draw hook redirects
        // interface draws only while one is.
        bool hudContent = false;
        ID3D11Texture2D* hud = UiHook::TakeHudFrame(&hudContent);
        bool hudWorldContent = false;
        ID3D11Texture2D* hudWorld = UiHook::TakeHudWorldFrame(&hudWorldContent);
        if constexpr (kEnableOpenXr)
        {
            // [PERF] Numpad5: no HUD/marker bridges this frame; the interface
            // stays in the game frame (consumer off -> draw detours early-out).
            const bool hudBypass = PerfCensus::bypassHud.load(std::memory_order_relaxed);
            RetailXr::SetHudLayer(hudBypass ? nullptr : hud, !hudBypass && hudContent);
            RetailXr::SetHudWorldLayer(hudBypass ? nullptr : hudWorld, !hudBypass && hudWorldContent);
            for(unsigned slot=0;slot<ObjectiveWorld::MaxMarkers;++slot) {
                ObjectiveWorld::Frame objective{};
                auto* objectiveTexture=UiHook::TakeObjectiveFrame(&objective,slot);
                if (hudBypass) { objective = ObjectiveWorld::Frame{}; objectiveTexture = nullptr; }
                RetailXr::SetObjectiveLayer(objectiveTexture,objective,slot);
            }
            ObjectiveWorld::Frame target{};
            auto* targetTexture=UiHook::TakeTargetFrame(&target);
            if (hudBypass) { target = ObjectiveWorld::Frame{}; targetTexture = nullptr; }
            RetailXr::SetTargetLayer(targetTexture,target);
            ObjectiveWorld::Frame interaction{};
            auto* interactionTexture=UiHook::TakeInteractionFrame(&interaction);
            if (hudBypass) { interaction = ObjectiveWorld::Frame{}; interactionTexture = nullptr; }
            RetailXr::SetInteractionLayer(interactionTexture,interaction);
            UiHook::SetHudWorldRouting(RetailXr::GetHudLensExact());
            {
                float uiP00 = 0.0f, uiP11 = 0.0f;
                if (UiHook::GetUiLens(&uiP00, &uiP11))
                    RetailXr::SetUiLens(uiP00, uiP11);
            }
            UiHook::SetHudLayerConsumer(!hudBypass && !NoHeadsetMode() && RetailXr::IsHudLayerConsuming());
        }
        else
            UiHook::SetHudLayerConsumer(false);
    }
    // The mod's own draws below (tuner, capture) are neither counted nor fitted.
    UiHook::SetModRender(true);
    // Bake the tuner into the live backbuffer before OpenXR
    // captures the current AER eye, making the menu and software cursor visible
    // in both retained headset images after one left/right pair.
    AerMenu::OnPresent(swapChain);
    if constexpr (kEnableOpenXr)
    {
        if (!NoHeadsetMode() && !PerfCensus::bypassXr.load(std::memory_order_relaxed)) // [PERF] Numpad9
        {
            RetailXr::SetSplitMonoFallback(PresenterMonoFallback());
            RetailXr::SetFlatPresents(EyeBuildQuietPresents());
            RetailXr::OnPresent(swapChain,
                g_stereoEnabled.load(std::memory_order_acquire),
                g_aerEnabled.load(std::memory_order_acquire));
        }
    }
    UiHook::SetModRender(false);
    const long long beforePresentQpc = FrameQpc();
    FpsAbProbe::BeforePresent(swapChain);
    bool desktopFallback=false;
    HRESULT result = E_FAIL;
    {
        // [PERF] Numpad4 desktop present mode: 0 normal, 1 DXGI_PRESENT_DO_NOT_WAIT
        // (never block; a not-ready present is dropped and reported), 2 skip
        // (one desktop present in 120 keeps the window alive).
        const int presentMode = NoHeadsetMode() ? 0 : PerfCensus::presentMode.load(std::memory_order_relaxed);
        static unsigned skipCounter = 0;
        if (presentMode == 2 && (skipCounter++ % 120) != 0) { PerfCensus::Add(PerfCensus::PresentSkipped); result = S_OK; }
        else if (g_realPresent)
        {
            const UINT adjusted = presentMode == 1 ? (flags | DXGI_PRESENT_DO_NOT_WAIT) : flags;
            result = PresentTiming::Measure(PresentTiming::Desktop,[&]{return DesktopPresentPolicy::Present([&](UINT f){return g_realPresent(swapChain,syncInterval,f);},originalFlags,adjusted,&desktopFallback);});
            if (result == DXGI_ERROR_WAS_STILL_DRAWING) { PerfCensus::Add(PerfCensus::PresentNotReady); result = S_OK; }
        }
    }
    const long long afterPresentQpc = FrameQpc();
    if(desktopFallback) {static std::atomic_bool reported{};if(!reported.exchange(true)) Log("[DESKTOPPACE] optional present flag rejected; original flags used");}
    NoteFrameTiming(frameEntryQpc, beforePresentQpc, afterPresentQpc);
    PerfCensus::OnPresentDone(frameEntryQpc, beforePresentQpc, afterPresentQpc, g_stereoEnabled.load(std::memory_order_acquire), g_aerEnabled.load(std::memory_order_acquire));
    FpsAbProbe::AfterPresent(g_stereoEnabled.load(std::memory_order_acquire),
        g_aerEnabled.load(std::memory_order_acquire), RetailXr::GetSceneIsFlat(),
        PresenterMonoFallback(),
        frameEntryQpc, beforePresentQpc, afterPresentQpc, RetailXr::GetLastXrWaitMicros());
    if constexpr (ResearchDiagnostics::Enabled) GpuPoseTrace::Poll();
    RetailXr::EndHeadPosePresent();
    return result;
}

HRESULT HookPresent1(Present1Fn g_realPresent1, IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    const UINT originalFlags=flags;
    if(!NoHeadsetMode()) flags=DesktopPresentPolicy::PresentFlags(swapChain,syncInterval,flags);
    PresentTiming::Frame timingFrame(swapChain,syncInterval,flags,g_stereoEnabled.load()?1u:(g_aerEnabled.load()?2u:0u));
    const long long frameEntryQpc = FrameQpc();
    PresentProfiler::NotePresentThread(GetCurrentThreadId());
    PerfCensus::presentTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    RetailXr::BeginHeadPosePresent();
    Tick();
    WatchContextVtables(swapChain);   // [VTWATCH]
    GBufferCensus::OnPresent(swapChain, g_aerEnabled.load(std::memory_order_acquire));
    DlssProbe::OnPresent(g_aerEnabled.load(std::memory_order_acquire),
        g_stereoEnabled.load(std::memory_order_relaxed), AerControl::GetRenderEye());
    DlssProbe::OnPresentGpu(swapChain);
    DlssUpscaler::OnPresent();
    UiHook::SetNativeStereo(g_stereoEnabled.load(std::memory_order_relaxed));
    UiHook::SetWorldIcons(VrCameraEnabled());   // [AERICONS]
    UiHook::OnPresent(swapChain);
    {
        // [HUDLAYER] Hand this frame's interface image to the presenter and
        // learn whether a headset is consuming it; the draw hook redirects
        // interface draws only while one is.
        bool hudContent = false;
        ID3D11Texture2D* hud = UiHook::TakeHudFrame(&hudContent);
        bool hudWorldContent = false;
        ID3D11Texture2D* hudWorld = UiHook::TakeHudWorldFrame(&hudWorldContent);
        if constexpr (kEnableOpenXr)
        {
            // [PERF] Numpad5: no HUD/marker bridges this frame; the interface
            // stays in the game frame (consumer off -> draw detours early-out).
            const bool hudBypass = PerfCensus::bypassHud.load(std::memory_order_relaxed);
            RetailXr::SetHudLayer(hudBypass ? nullptr : hud, !hudBypass && hudContent);
            RetailXr::SetHudWorldLayer(hudBypass ? nullptr : hudWorld, !hudBypass && hudWorldContent);
            for(unsigned slot=0;slot<ObjectiveWorld::MaxMarkers;++slot) {
                ObjectiveWorld::Frame objective{};
                auto* objectiveTexture=UiHook::TakeObjectiveFrame(&objective,slot);
                if (hudBypass) { objective = ObjectiveWorld::Frame{}; objectiveTexture = nullptr; }
                RetailXr::SetObjectiveLayer(objectiveTexture,objective,slot);
            }
            ObjectiveWorld::Frame target{};
            auto* targetTexture=UiHook::TakeTargetFrame(&target);
            if (hudBypass) { target = ObjectiveWorld::Frame{}; targetTexture = nullptr; }
            RetailXr::SetTargetLayer(targetTexture,target);
            ObjectiveWorld::Frame interaction{};
            auto* interactionTexture=UiHook::TakeInteractionFrame(&interaction);
            if (hudBypass) { interaction = ObjectiveWorld::Frame{}; interactionTexture = nullptr; }
            RetailXr::SetInteractionLayer(interactionTexture,interaction);
            UiHook::SetHudWorldRouting(RetailXr::GetHudLensExact());
            {
                float uiP00 = 0.0f, uiP11 = 0.0f;
                if (UiHook::GetUiLens(&uiP00, &uiP11))
                    RetailXr::SetUiLens(uiP00, uiP11);
            }
            UiHook::SetHudLayerConsumer(!hudBypass && !NoHeadsetMode() && RetailXr::IsHudLayerConsuming());
        }
        else
            UiHook::SetHudLayerConsumer(false);
    }
    // The mod's own draws below (tuner, capture) are neither counted nor fitted.
    UiHook::SetModRender(true);
    // Bake the tuner into the live backbuffer before OpenXR
    // captures the current AER eye, making the menu and software cursor visible
    // in both retained headset images after one left/right pair.
    AerMenu::OnPresent(swapChain);
    if constexpr (kEnableOpenXr)
    {
        if (!NoHeadsetMode() && !PerfCensus::bypassXr.load(std::memory_order_relaxed)) // [PERF] Numpad9
        {
            RetailXr::SetSplitMonoFallback(PresenterMonoFallback());
            RetailXr::SetFlatPresents(EyeBuildQuietPresents());
            RetailXr::OnPresent(swapChain,
                g_stereoEnabled.load(std::memory_order_acquire),
                g_aerEnabled.load(std::memory_order_acquire));
        }
    }
    UiHook::SetModRender(false);
    const long long beforePresentQpc = FrameQpc();
    FpsAbProbe::BeforePresent(swapChain);
    bool desktopFallback=false;
    HRESULT result = E_FAIL;
    {
        // [PERF] Numpad4 desktop present mode: 0 normal, 1 DXGI_PRESENT_DO_NOT_WAIT
        // (never block; a not-ready present is dropped and reported), 2 skip
        // (one desktop present in 120 keeps the window alive).
        const int presentMode = NoHeadsetMode() ? 0 : PerfCensus::presentMode.load(std::memory_order_relaxed);
        static unsigned skipCounter = 0;
        if (presentMode == 2 && (skipCounter++ % 120) != 0) { PerfCensus::Add(PerfCensus::PresentSkipped); result = S_OK; }
        else if (g_realPresent1)
        {
            const UINT adjusted = presentMode == 1 ? (flags | DXGI_PRESENT_DO_NOT_WAIT) : flags;
            result = PresentTiming::Measure(PresentTiming::Desktop,[&]{return DesktopPresentPolicy::Present([&](UINT f){return g_realPresent1(swapChain,syncInterval,f,parameters);},originalFlags,adjusted,&desktopFallback);});
            if (result == DXGI_ERROR_WAS_STILL_DRAWING) { PerfCensus::Add(PerfCensus::PresentNotReady); result = S_OK; }
        }
    }
    const long long afterPresentQpc = FrameQpc();
    if(desktopFallback) {static std::atomic_bool reported{};if(!reported.exchange(true)) Log("[DESKTOPPACE] optional present flag rejected; original flags used");}
    NoteFrameTiming(frameEntryQpc, beforePresentQpc, afterPresentQpc);
    PerfCensus::OnPresentDone(frameEntryQpc, beforePresentQpc, afterPresentQpc, g_stereoEnabled.load(std::memory_order_acquire), g_aerEnabled.load(std::memory_order_acquire));
    FpsAbProbe::AfterPresent(g_stereoEnabled.load(std::memory_order_acquire),
        g_aerEnabled.load(std::memory_order_acquire), RetailXr::GetSceneIsFlat(),
        PresenterMonoFallback(),
        frameEntryQpc, beforePresentQpc, afterPresentQpc, RetailXr::GetLastXrWaitMicros());
    if constexpr (ResearchDiagnostics::Enabled) GpuPoseTrace::Poll();
    RetailXr::EndHeadPosePresent();
    return result;
}

using ResizeBuffersFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);

HRESULT HookResizeBuffers(ResizeBuffersFn g_realResizeBuffers, IDXGISwapChain* c,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags) {
    return g_realResizeBuffers ? g_realResizeBuffers(c,n,w,h,f,DesktopPresentPolicy::ResizeFlags(c,flags)) : E_FAIL;
}

void HookSwapChain(IDXGISwapChain* swapChain)
{
    if (!swapChain)
        return;
    InstallCommandListProbe(swapChain);
    void** table = *reinterpret_cast<void***>(swapChain);


    DXGI_SWAP_CHAIN_DESC actual{};
    if(SUCCEEDED(swapChain->GetDesc(&actual)))
        Log("[DESKTOPPACE] swapchain effect=%u flags=0x%X windowed=%d tearing=%d bufferCount=%u",unsigned(actual.SwapEffect),actual.Flags,actual.Windowed,(actual.Flags&DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)!=0,actual.BufferCount);
    {
        IDXGIDevice1* dxgiDevice1{};
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDevice1))) && dxgiDevice1)
        {
            UINT latency = 0;
            const HRESULT hr = dxgiDevice1->GetMaximumFrameLatency(&latency);
            Log("[PERF] game device maximum frame latency=%u hr=0x%08X", latency, static_cast<unsigned>(hr));
            dxgiDevice1->Release();
        }
        PerfCensus::LoadIni();
        PerfCensus::LogSwitches("startup");
        Log("[PERF] census: [PERF] summary every 2 s; ini [Perf] Timers=1 adds per-call timers, [Perf] Sampler=1 arms the [PROFILE] sampler in native stereo. [SHADOWFIX]+[RTCACHE]+[TYPECACHE]+[TRACKSET] trims active.");
    }
    void* present1Target=nullptr;
    IDXGISwapChain1* swapChain1{};
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))))
    {
        void** table1 = *reinterpret_cast<void***>(swapChain1);
        present1Target=table1[22];
        swapChain1->Release();
    }
    SwapchainInline::Install(table[8],present1Target,table[13],HookPresent,HookPresent1,HookResizeBuffers,Log);
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain)
{
    if (desc)
        DisplaySpoof::NoteSwapchain("CreateSwapChain", desc->BufferDesc.Width,
            desc->BufferDesc.Height, static_cast<unsigned>(desc->BufferDesc.Format),
            desc->Windowed ? 1 : 0, desc->OutputWindow);
    DXGI_SWAP_CHAIN_DESC adjusted{}; auto* used=desc;
    if(desc && !NoHeadsetMode()) {adjusted=*desc;adjusted.Flags=DesktopPresentPolicy::CreationFlags(factory,desc->SwapEffect,!!desc->Windowed,desc->Flags);used=&adjusted;}
    HRESULT result=g_realCreateSwapChain ? g_realCreateSwapChain(factory,device,used,swapChain) : E_FAIL;
    if((result==DXGI_ERROR_INVALID_CALL || result==DXGI_ERROR_UNSUPPORTED || result==E_INVALIDARG) && desc && used!=desc && adjusted.Flags!=desc->Flags && g_realCreateSwapChain)
        result=g_realCreateSwapChain(factory,device,desc,swapChain);
    if (SUCCEEDED(result) && swapChain)
        HookSwapChain(*swapChain);
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain)
{
    if (desc)
        DisplaySpoof::NoteSwapchain("CreateSwapChainForHwnd", desc->Width, desc->Height,
            static_cast<unsigned>(desc->Format), (!fullscreen || fullscreen->Windowed) ? 1 : 0,
            window);
    DXGI_SWAP_CHAIN_DESC1 adjusted{}; const auto* used=desc;
    if(desc && !NoHeadsetMode()) {adjusted=*desc;adjusted.Flags=DesktopPresentPolicy::CreationFlags(factory,desc->SwapEffect,!fullscreen || fullscreen->Windowed,desc->Flags);used=&adjusted;}
    HRESULT result=g_realCreateSwapChainForHwnd ? g_realCreateSwapChainForHwnd(factory,device,window,used,fullscreen,restrictToOutput,swapChain) : E_FAIL;
    if((result==DXGI_ERROR_INVALID_CALL || result==DXGI_ERROR_UNSUPPORTED || result==E_INVALIDARG) && desc && used!=desc && adjusted.Flags!=desc->Flags && g_realCreateSwapChainForHwnd)
        result=g_realCreateSwapChainForHwnd(factory,device,window,desc,fullscreen,restrictToOutput,swapChain);
    if (SUCCEEDED(result) && swapChain)
        HookSwapChain(*swapChain);
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device,
    IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput,
    IDXGISwapChain1** swapChain)
{
    const HRESULT result = g_realCreateSwapChainForCoreWindow
        ? g_realCreateSwapChainForCoreWindow(factory, device, window, desc, restrictToOutput, swapChain) : E_FAIL;
    if (SUCCEEDED(result) && swapChain)
        HookSwapChain(*swapChain);
    return result;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain)
{
    const HRESULT result = g_realCreateSwapChainForComposition
        ? g_realCreateSwapChainForComposition(factory, device, desc, restrictToOutput, swapChain) : E_FAIL;
    if (SUCCEEDED(result) && swapChain)
        HookSwapChain(*swapChain);
    return result;
}

void HookFactory(IUnknown* factoryUnknown)
{
    if (!factoryUnknown)
        return;
    DisplaySpoof::HookDxgiOutputs(factoryUnknown);
    IDXGIFactory* factory{};
    if (SUCCEEDED(factoryUnknown->QueryInterface(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))))
    {
        void** table = *reinterpret_cast<void***>(factory);
        PatchVtable(table, 10, reinterpret_cast<void*>(&HookCreateSwapChain),
            reinterpret_cast<void**>(&g_realCreateSwapChain));
        factory->Release();
    }
    IDXGIFactory2* factory2{};
    if (SUCCEEDED(factoryUnknown->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))))
    {
        void** table = *reinterpret_cast<void***>(factory2);
        PatchVtable(table, 15, reinterpret_cast<void*>(&HookCreateSwapChainForHwnd),
            reinterpret_cast<void**>(&g_realCreateSwapChainForHwnd));
        PatchVtable(table, 16, reinterpret_cast<void*>(&HookCreateSwapChainForCoreWindow),
            reinterpret_cast<void**>(&g_realCreateSwapChainForCoreWindow));
        PatchVtable(table, 24, reinterpret_cast<void*>(&HookCreateSwapChainForComposition),
            reinterpret_cast<void**>(&g_realCreateSwapChainForComposition));
        factory2->Release();
    }
}

bool LoadRealDxgi()
{
    if (g_realDxgi)
        return true;
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    wchar_t path[MAX_PATH]{};
    constexpr wchar_t suffix[] = L"\\dxgi.dll";
    if (static_cast<std::size_t>(length) + _countof(suffix) > _countof(path))
        return false;
    std::memcpy(path, systemDirectory, length * sizeof(wchar_t));
    std::memcpy(path + length, suffix, sizeof(suffix));
    g_realDxgi = LoadLibraryW(path);
    return g_realDxgi != nullptr;
}

FARPROC RealProcedure(const char* name)
{
    return LoadRealDxgi() ? GetProcAddress(g_realDxgi, name) : nullptr;
}

DWORD WINAPI InitializeThread(void*)
{
    OpenLog();
    Log("FFXV VR proxy starting (build %s %s)", __DATE__, __TIME__);
    Log("F9: native stereo; AER in menu; automatic camera-writer recovery");
    DisplaySpoof::FlushEarlyLog();
    PerfCensus::LoadIni();
    if (PerfCensus::sampler.load()) PresentProfiler::Start();
    LoadRealDxgi();
    if (!ValidateRetailExecutable())
    {
        Log("unsupported executable: expected timestamp=0x%08X imageSize=0x%08X",
            kRetailTimestamp, kRetailImageSize);
        return 0;
    }
    Log("retail executable verified: base=%p", reinterpret_cast<void*>(g_exeBase));
    SteamCallbackLimit::Start(g_self, WriteLogLine);
    for (unsigned iteration = 0; iteration < 120000; ++iteration)
    {
        if (InstallEnginePatches())
            return 0;
        Sleep(iteration < 10000 ? 0 : 1);
    }
    Log("engine patch timeout: a required live prologue never matched");
    return 0;
}
} // namespace

namespace AerControl
{
bool IsAerEnabled()
{
    return g_aerEnabled.load(std::memory_order_acquire);
}

void SetAerEnabled(bool enabled)
{
    if (enabled)
        RetailXr::SetMonoSubmit(false);   // real AER, not Mono
    SetAer(enabled);
}

bool IsMonoEnabled()
{
    return g_aerEnabled.load(std::memory_order_acquire) && RetailXr::GetMonoSubmit();
}

void SetMonoEnabled(bool enabled)
{
    SetMono(enabled);
}

int GetPreferredVrMode()
{
    return g_preferredVrMode.load(std::memory_order_relaxed);
}
void SetPreferredVrMode(int mode)
{
    if (mode >= 1 && mode <= 3)
        g_preferredVrMode.store(mode, std::memory_order_relaxed);
}
int GetMonoKey()
{
    return g_monoKey.load(std::memory_order_relaxed);
}
void SetMonoKey(int vk)
{
    if (vk > 0 && g_monoKey.exchange(vk, std::memory_order_relaxed) != vk)
        g_monoKeyDown.store(true, std::memory_order_release);   // the bind press itself must not fire
}
bool GetDecoupledPitch()
{
    return g_decoupledPitch.load(std::memory_order_relaxed);
}
void SetDecoupledPitch(bool enabled)
{
    if (g_decoupledPitch.exchange(enabled, std::memory_order_relaxed) != enabled)
        Log("[DECOUPLEPITCH] %s", enabled ? "ON: only the head pitches the view" : "OFF");
}
int GetVrEnableKey()
{
    return g_vrEnableKey.load(std::memory_order_relaxed);
}
void SetVrEnableKey(int vk)
{
    if (vk > 0 && g_vrEnableKey.exchange(vk, std::memory_order_relaxed) != vk)
        g_f9Down.store(true, std::memory_order_release);   // the bind press itself must not fire
}
bool GetDeveloperKeysEnabled()
{
    return DeveloperKeysEnabled();
}

float GetHalfEyeMeters()
{
    return g_aerHalfEyeMeters.load(std::memory_order_relaxed);
}

void SetHalfEyeMeters(float meters)
{
    g_aerHalfEyeMeters.store(std::clamp(meters, 0.010f, 0.090f),
        std::memory_order_relaxed);
}

bool GetAfwEnabled()
{
    return g_afwEnabled.load(std::memory_order_acquire);
}

void SetAfwEnabled(bool enabled)
{
    g_afwEnabled.store(enabled, std::memory_order_release);
    RetailXr::SetAfwEnabled(enabled);
    Log("[AFW] %s", enabled ? "ON: the unrendered eye is synthesized from the rendered one each frame (AER only)"
                            : "off: AER shows the retained eye");
}

bool GetAfwFlip()
{
    return g_afwFlip.load(std::memory_order_acquire);
}

void SetAfwFlip(bool flip)
{
    g_afwFlip.store(flip, std::memory_order_release);
    RetailXr::SetAfwFlip(flip);
    Log("[AFWFLIP] synthesized eye is on the %s side", flip ? "flipped" : "default");
}

bool GetSwapEyes()
{
    return g_aerSwapEyes.load(std::memory_order_relaxed);
}

void SetSwapEyes(bool swap)
{
    g_aerSwapEyes.store(swap, std::memory_order_relaxed);
}

bool GetNativeSwapEyes()
{
    return RetailXr::GetSplitSwapEyes();
}

void SetNativeSwapEyes(bool swap)
{
    RetailXr::SetSplitSwapEyes(swap);
}

float GetNativeConvergence()
{
    return RetailXr::GetSplitConvergence();
}

void SetNativeConvergence(float tangentShift)
{
    RetailXr::SetSplitConvergence(tangentShift);
}

bool IsNativeStereoEnabled()
{
    return g_stereoEnabled.load(std::memory_order_acquire);
}

void SetNativeStereoEnabled(bool enabled)
{
    SetStereo(enabled);
}

float GetNativeBaselineCm()
{
    const float custom = g_nativeBaselineCm.load(std::memory_order_relaxed);
    if (custom > 0.0f)
        return custom;
    if (g_savedStereoData)
        return g_savedStereoBaseline;
    return 6.0f;
}

void SetNativeBaselineCm(float centimetres)
{
    const float value = centimetres > 0.0f ? std::clamp(centimetres, 1.0f, 40.0f) : 0.0f;
    g_nativeBaselineCm.store(value, std::memory_order_relaxed);
    if (g_stereoEnabled.load(std::memory_order_acquire))
        WriteNativeBaseline(value > 0.0f ? value : g_savedStereoBaseline);
}

bool GetSplitHonestLens()
{
    return RetailXr::GetSplitHonestLens();
}

float GetNativeFill()
{
    return RetailXr::GetSplitFill();
}

void SetNativeFill(float fill)
{
    RetailXr::SetSplitFill(fill);
}

bool GetNativeSymmetric()
{
    return g_nativeSymmetric.load(std::memory_order_acquire);
}

int GetConversationMode()
{
    return g_conversationMode.load(std::memory_order_acquire);
}

void SetConversationMode(int mode)
{
    g_conversationMode.store(std::clamp(mode, 0, 2), std::memory_order_release);
}

int GetConversationActive()
{
    return g_conversationActive.load(std::memory_order_acquire);
}

void SetNativeSymmetric(bool enabled)
{
    g_nativeSymmetric.store(enabled, std::memory_order_release);
}

void SetSplitHonestLens(bool enabled)
{
    RetailXr::SetSplitHonestLens(enabled);
}

float GetConvergence()
{
    return RetailXr::GetAerConvergence();
}

void SetConvergence(float tangentShift)
{
    RetailXr::SetAerConvergence(std::clamp(tangentShift, -0.25f, 0.25f));
}

float GetFillFactor()
{
    return RetailXr::GetFillFactor();
}

void SetFillFactor(float fill)
{
    RetailXr::SetFillFactor(std::clamp(fill, 0.35f, 2.0f));
}

bool GetLiveFovOverrideEnabled()
{
    return g_liveFovOverrideEnabled.load(std::memory_order_acquire);
}

void SetLiveFovOverrideEnabled(bool enabled)
{
    const bool previous = g_liveFovOverrideEnabled.exchange(
        enabled, std::memory_order_acq_rel);
    if (previous == enabled)
        return;
    Log("[IVFOV] render-bound FOV override %s at %.1f degrees",
        enabled ? "ON" : "OFF",
        g_liveFovDegrees.load(std::memory_order_relaxed));
    if (!enabled)
    {
        RestoreGameFov();
        IvFovDumpState("toggle-off");
    }
}

float GetLiveFovDegrees()
{
    return g_liveFovDegrees.load(std::memory_order_relaxed);
}

int GetFovParamIndex()
{
    return g_camParamFovIndex.load(std::memory_order_relaxed);
}

void SetFovParamIndex(int index)
{
    if (index >= -1 && index < static_cast<int>(kCamParamCount))
    {
        g_camParamFovIndex.store(index, std::memory_order_release);
        Log("[CAMPARAM] FOV slot set to %d by configuration", index);
    }
}

int GetFovSinkOffset()
{
    return g_fovSinkOffset.load(std::memory_order_acquire);
}

int GetFovSinkKind()
{
    return static_cast<int>(g_fovSinkKind.load(std::memory_order_acquire));
}

void SetFovSink(int offset, int kind)
{
    if (offset >= 0 && static_cast<std::uint32_t>(offset) + 4 <= kFovSinkScanBytes &&
        kind > 0 && kind <= 4 &&
        g_fovSinkOffset.load(std::memory_order_acquire) < 0)
    {
        g_fovSinkKind.store(static_cast<std::uint8_t>(kind),
            std::memory_order_release);
        g_fovSinkOffset.store(offset, std::memory_order_release);
        Log("[FOVSINK] restored from settings: camera+0x%X kind=%d", offset,
            kind);
    }
}

bool GetLensFeedEnabled()
{
    return g_lensFeedEnabled.load(std::memory_order_acquire);
}

void SetLensFeedEnabled(bool enabled)
{
    if (g_lensFeedEnabled.exchange(enabled, std::memory_order_acq_rel) !=
        enabled)
    {
        Log("[LENSFEED] headset FOV tracking of the rendered lens %s "
            "(writes=%llu)",
            enabled ? "ON" : "OFF",
            static_cast<unsigned long long>(
                g_lensFeedWrites.load(std::memory_order_relaxed)));
        if (!enabled)
            RetailXr::SetGameProjection(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

bool GetEyeHoldEnabled()
{
    return g_eyeHoldEnabled.load(std::memory_order_acquire);
}

bool GetAerFreshHeadPose()
{
    return g_aerFreshHeadPose.load(std::memory_order_relaxed);
}

void SetAerFreshHeadPose(bool enabled)
{
    const bool wasFresh = g_aerFreshHeadPose.exchange(false, std::memory_order_relaxed);
    if (enabled || wasFresh)
        Log("[AERFRESH] fresh second-eye pose rejected: AER pairs use the first eye's rendered pose");
}

bool GetAerSimSync()
{
    return g_aerSimSync.load(std::memory_order_relaxed);
}

void SetAerSimSync(bool enabled)
{
    if (g_aerSimSync.exchange(enabled, std::memory_order_relaxed) != enabled)
        Log("[AERSYNC] game-moment sync across eye pairs %s", enabled ? "enabled" : "disabled");
}

void SetEyeHoldEnabled(bool enabled)
{
    if (g_eyeHoldEnabled.exchange(enabled, std::memory_order_acq_rel) != enabled)
        Log("[EYEHOLD] camera hold across eye pairs %s",
            enabled ? "ON" : "OFF");
}

bool GetFovAllModes()
{
    return g_camParamAllModes.load(std::memory_order_acquire);
}

void SetFovAllModes(bool enabled)
{
    if (g_camParamAllModes.exchange(enabled, std::memory_order_acq_rel) !=
        enabled)
        Log("[CAMPARAM] all camera modes %s", enabled ? "ON" : "OFF");
}

int GetFovParamValue()
{
    const auto index = g_camParamFovIndex.load(std::memory_order_relaxed);
    if (index < 0)
        return -1;
    auto* table = CameraParamTable();
    if (!table)
        return -1;
    int value = -1;
    __try
    {
        value = static_cast<int>(CamParamAsFloat(table[index]));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return value;
}

// The engine REJECTS camera FOV values at or above ~90 degrees (pi/2 rad)
// and falls back to a narrow lens: log-proven, the hook injected
// 1.5708 rad (exactly 90) thousands of times with no fill, while 87-88
// fills instantly; a saved value of 90-100 would leave the headset unfilled.
// The ceiling keeps every injected value inside the accepted range.
constexpr float kEngineFovCeilingDegrees = 87.0f;

void SetLiveFovDegrees(float degrees)
{
    if (std::isfinite(degrees))
        g_liveFovDegrees.store(
            std::clamp(degrees, 30.0f, kEngineFovCeilingDegrees),
            std::memory_order_release);
}

float GetNativeHeadTiming() { return RetailXr::GetNativeHeadTiming(); }
void SetNativeHeadTiming(float offset) { RetailXr::SetNativeHeadTiming(offset); }
float GetHeadLookaheadFrames() { return RetailXr::GetHeadLookaheadFrames(); }
bool GetNativeTagCurrent() { return RetailXr::GetNativeTagCurrent(); }
void SetNativeTagCurrent(bool current) { RetailXr::SetNativeTagCurrent(current); }
void SetHeadLookaheadFrames(float frames) { RetailXr::SetHeadLookaheadFrames(frames); }
int GetHeadPipelineDepth() { return RetailXr::GetHeadPipelineDepth(); }

bool GetSixDofEnabled()
{
    return g_sixDofEnabled.load(std::memory_order_acquire);
}

void SetSixDofEnabled(bool enabled)
{
    g_sixDofEnabled.store(enabled, std::memory_order_release);
    RetailXr::SetSixDofCameraEnabled(enabled);
    Log("[6DOF] positional XYZ tracking %s (camera roll disabled)",
        enabled ? "ON" : "OFF");
}

float GetSixDofPositionScale()
{
    return g_sixDofPositionScale.load(std::memory_order_relaxed);
}

void SetSixDofPositionScale(float scale)
{
    if (std::isfinite(scale))
        g_sixDofPositionScale.store(std::clamp(scale, 0.0f, 3.0f),
            std::memory_order_release);
}

bool GetCustomFirstPersonEnabled()
{
    return g_customFirstPersonEnabled.load(std::memory_order_acquire);
}

void SetCustomFirstPersonEnabled(bool enabled)
{
    const bool previous = g_customFirstPersonEnabled.exchange(
        enabled, std::memory_order_acq_rel);
    if (previous != enabled)
        Log("[FPCAM] custom first-person camera %s", enabled ? "ON" : "OFF");
}

void GetCustomFirstPersonOffsets(float* right, float* up, float* forward)
{
    if (right)
        *right = g_customFirstPersonRight.load(std::memory_order_relaxed);
    if (up)
        *up = g_customFirstPersonUp.load(std::memory_order_relaxed);
    if (forward)
        *forward = g_customFirstPersonForward.load(std::memory_order_relaxed);
}

void SetCustomFirstPersonOffsets(float right, float up, float forward)
{
    if (!std::isfinite(right) || !std::isfinite(up) ||
        !std::isfinite(forward))
        return;
    g_customFirstPersonRight.store(std::clamp(right, -1.0f, 1.0f),
        std::memory_order_release);
    g_customFirstPersonUp.store(std::clamp(up, -1.0f, 1.0f),
        std::memory_order_release);
    g_customFirstPersonForward.store(std::clamp(forward, -5.0f, 5.0f),
        std::memory_order_release);
}

int GetRenderEye()
{
    return RetailXr::GetAerRenderEye();
}

bool GetUiFitEnabled()
{
    return UiHook::GetFitEnabled();
}

void SetUiFitEnabled(bool enabled)
{
    UiHook::SetFitEnabled(enabled);
}

void GetUiFit(float* scaleX, float* scaleY, float* offsetX, float* offsetY)
{
    if (scaleX)
        *scaleX = g_uiFitScaleX.load(std::memory_order_acquire);
    if (scaleY)
        *scaleY = g_uiFitScaleY.load(std::memory_order_acquire);
    if (offsetX)
        *offsetX = g_uiFitOffsetX.load(std::memory_order_acquire);
    if (offsetY)
        *offsetY = g_uiFitOffsetY.load(std::memory_order_acquire);
}

void SetUiFit(float scaleX, float scaleY, float offsetX, float offsetY)
{
    g_uiFitScaleX.store(std::clamp(scaleX, 0.30f, 1.20f), std::memory_order_release);
    g_uiFitScaleY.store(std::clamp(scaleY, 0.30f, 1.20f), std::memory_order_release);
    g_uiFitOffsetX.store(std::clamp(offsetX, -0.35f, 0.35f), std::memory_order_release);
    g_uiFitOffsetY.store(std::clamp(offsetY, -0.35f, 0.35f), std::memory_order_release);
    UiHook::SetFit(scaleX, scaleY, offsetX, offsetY);
}

unsigned long long GetUiFitDrawCount()
{
    // The transform lives in ui_hook.cpp, on the d3d11.dll implementations.
    return UiHook::GetFitDrawCount();
}

bool GetHudLayerEnabled()
{
    return UiHook::GetHudLayerEnabled();
}

void SetHudLayerEnabled(bool enabled)
{
    UiHook::SetHudLayerEnabled(enabled);
    RetailXr::SetHudLayerEnabled(enabled);
}

void GetHudLayerParams(float* distanceMeters, float* widthMeters, float* verticalOffsetMeters)
{
    RetailXr::GetHudLayerParams(distanceMeters, widthMeters, verticalOffsetMeters);
}

void SetHudLayerParams(float distanceMeters, float widthMeters, float verticalOffsetMeters)
{
    RetailXr::SetHudLayerParams(distanceMeters, widthMeters, verticalOffsetMeters);
}

bool GetHudLensExact()
{
    return RetailXr::GetHudLensExact();
}

void SetHudLensExact(bool enabled)
{
    RetailXr::SetHudLensExact(enabled);
}

void SetHudWorldDistance(float meters)
{
    RetailXr::SetHudWorldDistance(meters);
}

float GetHudWorldDistance()
{
    return RetailXr::GetHudWorldDistance();
}

void SetHudWorldBaseCamera(bool enabled)
{
    RetailXr::SetHudWorldBaseCamera(enabled);
}

bool GetHudWorldBaseCamera()
{
    return RetailXr::GetHudWorldBaseCamera();
}

unsigned long long GetHudLayerDrawCount()
{
    return UiHook::GetHudRedirectCount();
}

bool GetFlatSceneMonoEnabled()
{
    return RetailXr::GetFlatSceneMonoEnabled();
}

void SetFlatSceneMonoEnabled(bool enabled)
{
    RetailXr::SetFlatSceneMonoEnabled(enabled);
}

bool GetSceneIsFlat()
{
    return RetailXr::GetSceneIsFlat();
}

void RequestLensSnapshot()
{
    g_lensSnapshot.store(true, std::memory_order_release);
}
void Recenter()
{
    RetailXr::ResetHeadPoseCenter();
}
} // namespace AerControl

void RetailLogLine(const char* message)
{
    WriteLogLine(message);
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = reinterpret_cast<Fn>(RealProcedure("CreateDXGIFactory"));
    const HRESULT result = fn ? fn(riid, factory) : E_FAIL;
    if (SUCCEEDED(result) && factory)
        HookFactory(reinterpret_cast<IUnknown*>(*factory));
    return result;
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = reinterpret_cast<Fn>(RealProcedure("CreateDXGIFactory1"));
    const HRESULT result = fn ? fn(riid, factory) : E_FAIL;
    if (SUCCEEDED(result) && factory)
        HookFactory(reinterpret_cast<IUnknown*>(*factory));
    return result;
}

extern "C" HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(RealProcedure("CreateDXGIFactory2"));
    const HRESULT result = fn ? fn(flags, riid, factory) : E_FAIL;
    if (SUCCEEDED(result) && factory)
        HookFactory(reinterpret_cast<IUnknown*>(*factory));
    return result;
}

extern "C" HRESULT WINAPI Proxy_DXGIDeclareAdapterRemovalSupport()
{
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(RealProcedure("DXGIDeclareAdapterRemovalSupport"));
    return fn ? fn() : E_NOTIMPL;
}

extern "C" void WINAPI Proxy_DXGIDisableVBlankVirtualization()
{
    using Fn = void(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(RealProcedure("DXGIDisableVBlankVirtualization"));
    if (fn)
        fn();
}

extern "C" HRESULT WINAPI Proxy_DXGIGetDebugInterface(REFIID riid, void** debug)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = reinterpret_cast<Fn>(RealProcedure("DXGIGetDebugInterface"));
    return fn ? fn(riid, debug) : E_NOINTERFACE;
}

extern "C" HRESULT WINAPI Proxy_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(RealProcedure("DXGIGetDebugInterface1"));
    return fn ? fn(flags, riid, debug) : E_NOINTERFACE;
}

extern "C" HRESULT WINAPI Proxy_DXGIReportAdapterConfiguration(DWORD flags)
{
    using Fn = HRESULT(WINAPI*)(DWORD);
    auto fn = reinterpret_cast<Fn>(RealProcedure("DXGIReportAdapterConfiguration"));
    return fn ? fn(flags) : E_NOTIMPL;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        // [INIRENAME] the settings file is ffxv-vr.ini; an install that still
        // has the old ffxv_aer.ini keeps its settings (renamed, never merged).
        {
            wchar_t oldPath[MAX_PATH]{}, newPath[MAX_PATH]{};
            if (GetModuleFileNameW(module, oldPath, MAX_PATH))
            {
                if (wchar_t* slash = std::wcsrchr(oldPath, L'\\'))
                {
                    slash[1] = L'\0';
                    std::wcscpy(newPath, oldPath);
                    if (std::wcslen(oldPath) + 16 < MAX_PATH)
                    {
                        std::wcscat(oldPath, L"ffxv_aer.ini");
                        std::wcscat(newPath, L"ffxv-vr.ini");
                        if (GetFileAttributesW(newPath) == INVALID_FILE_ATTRIBUTES &&
                            GetFileAttributesW(oldPath) != INVALID_FILE_ATTRIBUTES)
                            MoveFileW(oldPath, newPath);
                    }
                }
            }
        }
        // [DISPQ] must precede the game's first display query, which happens
        // on the main thread before the init thread could possibly run.
        DisplaySpoof::InstallEarly(module);
        if (HANDLE thread = CreateThread(nullptr, 0, InitializeThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_stereoEnabled.load(std::memory_order_acquire) && g_savedStereoData &&
            g_stereoEnableAddress && g_stereoSeparationAddress)
        {
            RestoreGameFov();
            *g_stereoEnableAddress = g_savedStereoEnable;
            *g_stereoSeparationAddress = g_savedStereoSeparation;
            if (g_stereoBaselineAddress)
                *g_stereoBaselineAddress = g_savedStereoBaseline;
            if (g_stereoConvergenceAddress)
                *g_stereoConvergenceAddress = g_savedStereoConvergence;
        }
        g_engineD3dTable.store(0, std::memory_order_release);
        const auto tableCount = std::min<std::uint32_t>(
            g_engineD3dTableCount.exchange(0, std::memory_order_acq_rel),
            static_cast<std::uint32_t>(kEngineD3dTableSlots));
        for (std::uint32_t tableIndex = 0; tableIndex < tableCount; ++tableIndex)
        {
            const auto engineTable =
                g_engineD3dTables[tableIndex].exchange(0, std::memory_order_acq_rel);
            if (!engineTable)
                continue;
            __try
            {
                auto* table = reinterpret_cast<std::uintptr_t*>(engineTable);
                if (g_realEngineVsSetConstantBuffers)
                    table[0x38 / 8] = reinterpret_cast<std::uintptr_t>(
                        g_realEngineVsSetConstantBuffers);
                if (g_realEngineDrawIndexed)
                    table[0x60 / 8] = reinterpret_cast<std::uintptr_t>(g_realEngineDrawIndexed);
                if (g_realEngineDrawIndexedInstanced)
                    table[0xA0 / 8] =
                        reinterpret_cast<std::uintptr_t>(g_realEngineDrawIndexedInstanced);
                if (g_realEngineDrawInstanced)
                    table[0xA8 / 8] =
                        reinterpret_cast<std::uintptr_t>(g_realEngineDrawInstanced);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        RestoreCodePatch(g_updateDispatchPatch);
        RestoreCodePatch(g_qualityMaskPatch);
        RestoreCodePatch(g_probeWeightsPatch);
        RestoreCodePatch(g_fiberOutputPatch);
        if (g_fiberOutputRelay)
        {
            VirtualFree(g_fiberOutputRelay, 0, MEM_RELEASE);
            g_fiberOutputRelay = nullptr;
        }
        RestoreCodePatch(g_passRegistrarPatch);
        RestoreCodePatch(g_bankRotatorPatch);
        for (auto& patch : g_postChainPatches)
            RestoreCodePatch(patch);
        RestoreCodePatch(g_multiOutputPatch);
        RestoreCodePatch(g_outputGetterPatch);
        RestoreCodePatch(g_matrixCachePatch);
        RestoreCodePatch(g_updateRenderViewPatch);
        RestoreCodePatch(g_poseBufferRecorderPatch);
        RestoreCodePatch(g_uploadMatricesPatch);
        RestoreCodePatch(g_eyeViewBuilderCallPatch);
        RestoreCodePatch(g_eyeProjectionBuilderCallPatch);
        RestoreCodePatch(g_sepProbeProjectionCallPatch);
        RestoreCodePatch(g_sepProbeViewCallPatch);
        if (g_eyeViewBuilderRelay)
        {
            VirtualFree(g_eyeViewBuilderRelay, 0, MEM_RELEASE);
            g_eyeViewBuilderRelay = nullptr;
        }
        if (g_eyeProjectionBuilderRelay)
        {
            VirtualFree(g_eyeProjectionBuilderRelay, 0, MEM_RELEASE);
            g_eyeProjectionBuilderRelay = nullptr;
        }
        if (g_sepProbeProjectionRelay)
        {
            VirtualFree(g_sepProbeProjectionRelay, 0, MEM_RELEASE);
            g_sepProbeProjectionRelay = nullptr;
        }
        if (g_sepProbeViewRelay)
        {
            VirtualFree(g_sepProbeViewRelay, 0, MEM_RELEASE);
            g_sepProbeViewRelay = nullptr;
        }
        RestoreCodePatch(g_passQueuePatch);
        RestoreCodePatch(g_replayCopyIatPatch);
        RestoreCodePatch(g_actorLodBudgetPatch);
        RestoreCodePatch(g_actorLodManagerRunPatch);
        RestoreCodePatch(g_actorPointLodKernelPatch);
        RestoreCodePatch(g_actorBoundedLodKernelPatch);
        RestoreCodePatch(g_replayDispatchCallPatch);
        RestoreCodePatch(g_replayMapCallPatch);
        RestoreCodePatch(g_replayDrawCallAPatch);
        RestoreCodePatch(g_replayDrawCallBPatch);
        if (g_replayDispatchRelay)
        {
            VirtualFree(g_replayDispatchRelay, 0, MEM_RELEASE);
            g_replayDispatchRelay = nullptr;
        }
        if (g_replayMapRelay)
        {
            VirtualFree(g_replayMapRelay, 0, MEM_RELEASE);
            g_replayMapRelay = nullptr;
        }
        if (g_replayDrawRelayA)
        {
            VirtualFree(g_replayDrawRelayA, 0, MEM_RELEASE);
            g_replayDrawRelayA = nullptr;
        }
        if (g_replayDrawRelayB)
        {
            VirtualFree(g_replayDrawRelayB, 0, MEM_RELEASE);
            g_replayDrawRelayB = nullptr;
        }
        if (g_logHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_logHandle);
            g_logHandle = INVALID_HANDLE_VALUE;
        }
        if (g_realDxgi)
            FreeLibrary(g_realDxgi);
    }
    return TRUE;
}
